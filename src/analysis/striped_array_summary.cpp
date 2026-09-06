// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/striped_array_summary.h"

#include <numeric>

namespace lshaz {

// Element boundaries sit at b + k*stride, so their offsets within a line
// step by gcd(stride, line) and can return to a line start only once every
// line/gcd values of k. Whatever b is, the remaining boundaries fall
// mid-line and are shared by the pair of elements that meet there.
static uint64_t straddledBoundaries(uint64_t elemCount, uint64_t elemSizeBytes,
                                    uint64_t lineBytes) {
    if (elemCount < 2 || lineBytes == 0) return 0;
    const uint64_t d = std::gcd(elemSizeBytes % lineBytes, lineBytes);
    if (d == 0) return 0;
    const uint64_t period = lineBytes / d;
    const uint64_t boundaries = elemCount - 1;
    const uint64_t maxAligned = (boundaries + period - 1) / period;
    return boundaries > maxAligned ? boundaries - maxAligned : 0;
}

StripeVerdict gradeStripedArray(const StripedArraySite &s,
                                const ThreadRoleVerdicts &roles,
                                uint64_t lineBytes) {
    StripeVerdict v;
    v.mitigation = classifyStripeMitigation(s, lineBytes);
    v.writerCount = static_cast<unsigned>(s.stripedWriters.size());

    if (s.elemSizeBytes > 0 && s.elemSizeBytes < lineBytes) {
        v.slotsPerLine = lineBytes / s.elemSizeBytes;
        v.contendedLines =
            (s.elemCount + v.slotsPerLine - 1) / v.slotsPerLine;
    } else if (strideStraddlesLines(s.elemSizeBytes, lineBytes)) {
        // An element wider than the line owns interior lines outright; only
        // the two elements meeting at a straddled boundary share one.
        v.slotsPerLine = 2;
        v.contendedLines =
            straddledBoundaries(s.elemCount, s.elemSizeBytes, lineBytes);
    }

    // Any unattributed writer collapses the mask: a partial attribution
    // cannot prove two roles touch this array.
    uint8_t mask = 0;
    for (const auto &w : s.stripedWriters) {
        uint8_t r = roles.roleOf(w);
        if (r == ROLE_NONE) { mask = ROLE_NONE; break; }
        mask |= r;
    }
    v.writerRoles = mask;
    v.multiRole = (mask == (ROLE_MAIN | ROLE_WORKER));
    // Every writer on the main thread means no two cores ever write this
    // array, whatever its subscript says.
    //
    // ROLE_WORKER alone is deliberately NOT included. A pool runs many
    // threads in that one role, so a single-role mask there is consistent
    // with heavy contention rather than evidence against it.
    v.mainThreadOnly = (mask == ROLE_MAIN);
    v.ownerIndexed =
        s.indexIsHandedOver && !s.indexIsOwnIdentity && !s.tlsIndexed;

    v.frequency = static_cast<WriteFrequencyTier>(s.writerTier);
    return v;
}

void applyStripeROI(StripeVerdict &v, const StripedArraySite &s,
                    uint64_t lineBytes, uint64_t l1dSizeBytes,
                    bool alignedOwnerAvailable) {
    // Round the stride up rather than assuming one line per slot: an
    // element already wider than a line pads to the next multiple, and
    // elemCount * lineBytes would underflow the difference below.
    const uint64_t padStride =
        lineBytes ? ((s.elemSizeBytes + lineBytes - 1) / lineBytes) * lineBytes
                  : s.elemSizeBytes;
    v.currentFootprint = s.elemCount * s.elemSizeBytes;
    v.paddedFootprint  = s.elemCount * padStride;
    v.l1dCostFraction =
        l1dSizeBytes ? static_cast<double>(v.paddedFootprint -
                                           v.currentFootprint) /
                           static_cast<double>(l1dSizeBytes)
                     : 0.0;

    const bool hot = v.frequency == WriteFrequencyTier::Hot;
    const bool expensive = v.l1dCostFraction > kFullPadL1DBudget;

    if (hot && !expensive) {
        v.fixShape = StripeFixShape::FullPad;
        v.fixRationale = "hot-path writes and full padding costs a small "
                         "share of L1D";
        return;
    }
    if (alignedOwnerAvailable) {
        // same isolation, zero added footprint, strictly dominates
        // padding wherever an aligned per-thread owner already exists.
        v.fixShape = StripeFixShape::RelocateToOwner;
        v.fixRationale = "an already line-aligned per-thread structure "
                         "exists; relocating costs no extra footprint";
        return;
    }
    // Head padding only shifts the base, which leaves a straddling stride
    // straddling, so the shapes below cannot be offered here. Rounding the
    // element up is the whole fix, and it costs the remainder rather than a
    // full line per slot, so it is worth naming well short of hot.
    if (strideStraddlesLines(s.elemSizeBytes, lineBytes) &&
        s.elemSizeBytes > lineBytes) {
        if (!expensive) {
            v.fixShape = StripeFixShape::FullPad;
            v.fixRationale = "rounding the element up to a line multiple is "
                             "the only shape that separates these slots, and "
                             "it costs a small share of L1D";
            return;
        }
        v.fixShape = StripeFixShape::None;
        v.fixRationale = "the stride is not a line multiple, so element "
                         "boundaries fall mid-line wherever the array sits, "
                         "and rounding the element up costs more L1D than "
                         "the coherence traffic it saves";
        return;
    }
    if (hot) {
        v.fixShape = StripeFixShape::HeadPad;
        v.fixRationale = "hot-path writes but full padding would consume a "
                         "large share of L1D; isolate the hottest slot only";
        return;
    }
    if (expensive) {
        v.fixShape = StripeFixShape::None;
        v.fixRationale = "coherence traffic is real but padding cost exceeds "
                         "the benefit at this write frequency";
        return;
    }
    v.fixShape = StripeFixShape::HeadPad;
    v.fixRationale = "cheap partial isolation; full padding is not justified "
                     "at this write frequency";
}

} // namespace lshaz
