// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/striped_array_summary.h"

namespace lshaz {

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

    v.frequency = static_cast<WriteFrequencyTier>(s.writerTier);
    return v;
}

void applyStripeROI(StripeVerdict &v, const StripedArraySite &s,
                    uint64_t lineBytes, uint64_t l1dSizeBytes,
                    bool alignedOwnerAvailable) {
    v.currentFootprint = s.elemCount * s.elemSizeBytes;
    v.paddedFootprint  = s.elemCount * lineBytes;
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
        // same isolation, zero added footprint — strictly dominates
        // padding wherever an aligned per-thread owner already exists.
        v.fixShape = StripeFixShape::RelocateToOwner;
        v.fixRationale = "an already line-aligned per-thread structure "
                         "exists; relocating costs no extra footprint";
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
