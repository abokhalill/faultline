// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/analysis/thread_role_summary.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace lshaz {

// Per-thread striped arrays: N slots, one per thread, packed
// floor(line/elemSize) per cache line. Elements i and j written by
// different cores trade the line in Modified state on every update.
//
// The gate is index provenance, not element atomicity: striping
// guarantees single-writer-per-element, so these arrays are routinely
// plain scalars. Requiring atomics misses the cleanest instances.
enum class IndexProvenance : uint8_t {
    Unknown       = 0,
    ConstantIdx   = 1,  // arr[0]        — one slot, not striping
    LoopInduction = 2,  // for(i..) arr[i] — aggregation sweep / bulk reset
    ThreadIdent   = 3,  // arr[thread_index], arr[c->running_tid]
};

// How often the striped write executes. Decides whether a real hazard is
// worth a real fix: padding that costs L1D footprint pays only against
// frequency.
enum class WriteFrequencyTier : uint8_t {
    Unknown  = 0,
    Tick     = 1,   // periodic / background
    Dispatch = 2,   // per-connection, per-assignment
    Hot      = 3,   // per-command, per-IO
};

constexpr const char *writeFrequencyName(WriteFrequencyTier t) {
    switch (t) {
        case WriteFrequencyTier::Hot:      return "hot";
        case WriteFrequencyTier::Dispatch: return "dispatch";
        case WriteFrequencyTier::Tick:     return "tick";
        default:                           return "unknown";
    }
}

struct StripedArraySite {
    std::string key;            // "<file>::<var>" (static) | "<Type>::<field>"
    std::string displayName;
    std::string typeName;       // enclosing record, empty for globals
    std::string file;
    unsigned    line = 0;
    uint64_t    elemSizeBytes = 0;
    uint64_t    elemCount     = 0;
    uint64_t    declAlignBytes = 0;
    bool elementIsAtomic   = false;   // booster, never a gate
    bool elementIsVolatile = false;
    bool isFileStatic      = false;
    // index is thread_local-derived: per-thread striping by definition.
    bool tlsIndexed        = false;
    // Base alignment alone separates slot 0 from the preceding symbol and
    // does nothing for slot 0 vs slot 1. Mitigation requires alignment
    // AND indexing that starts at a padded offset.
    bool hasHeadPaddingOffset = false;

    std::set<std::string> stripedWriters;  // thread-ident-indexed writers
    // Highest tier over all striped writers, resolved at collection time
    // where the oracle and the decl are both in scope. The fastest writer
    // is the binding constraint. Stored as the enum's underlying type so
    // it crosses the shard protocol as one integer.
    uint8_t writerTier = 0;
    std::set<std::string> aggregators;     // loop-swept readers/resetters

    void merge(const StripedArraySite &o) {
        elementIsAtomic       |= o.elementIsAtomic;
        elementIsVolatile     |= o.elementIsVolatile;
        tlsIndexed            |= o.tlsIndexed;
        hasHeadPaddingOffset  |= o.hasHeadPaddingOffset;
        if (elemSizeBytes == 0) { elemSizeBytes = o.elemSizeBytes;
                                  elemCount = o.elemCount;
                                  declAlignBytes = o.declAlignBytes; }
        if (file.empty()) { file = o.file; line = o.line;
                            displayName = o.displayName; typeName = o.typeName;
                            isFileStatic = o.isFileStatic; }
        stripedWriters.insert(o.stripedWriters.begin(), o.stripedWriters.end());
        writerTier = std::max(writerTier, o.writerTier);
        aggregators.insert(o.aggregators.begin(), o.aggregators.end());
    }
};

// Ordered: IPC byte sequence and reduce iteration must not depend on
// hash layout.
using StripedArraySummary = std::map<std::string, StripedArraySite>;

inline void mergeStripedArrays(StripedArraySummary &dst,
                               const StripedArraySummary &src) {
    for (const auto &[k, s] : src) {
        auto it = dst.find(k);
        if (it == dst.end()) dst.emplace(k, s);
        else                 it->second.merge(s);
    }
}

enum class StripeMitigation : uint8_t {
    None = 0,
    HeadPadded,           // slot 0 isolated, slots 1..N still pack
    FullyPadded,          // stride >= line: every slot owns a line
};

// stride, not data size: array indexing steps by the padded layout size.
inline StripeMitigation classifyStripeMitigation(const StripedArraySite &s,
                                                 uint64_t lineBytes) {
    if (lineBytes == 0) return StripeMitigation::None;
    if (s.elemSizeBytes != 0 && s.elemSizeBytes % lineBytes == 0)
        return StripeMitigation::FullyPadded;
    if (s.hasHeadPaddingOffset && s.declAlignBytes >= lineBytes)
        return StripeMitigation::HeadPadded;
    return StripeMitigation::None;
}

// What to actually do about it. Full padding buys isolation with L1D
// footprint; relocating into an existing line-aligned per-thread struct
// buys the same isolation for free. Recommending a fix the maintainer
// will reject is a finding with negative expected value.
enum class StripeFixShape : uint8_t {
    None = 0,           // true positive, no worthwhile fix
    RelocateToOwner,    // move into an existing aligned per-thread struct
    HeadPad,            // isolate slot 0 only; negligible footprint
    FullPad,            // one slot per line
};

constexpr const char *stripeFixName(StripeFixShape f) {
    switch (f) {
        case StripeFixShape::RelocateToOwner: return "relocate-to-aligned-owner";
        case StripeFixShape::HeadPad:         return "head-pad";
        case StripeFixShape::FullPad:         return "full-pad";
        default:                              return "none";
    }
}

struct StripeVerdict {
    uint8_t  writerRoles = ROLE_NONE;
    unsigned writerCount = 0;
    bool     multiRole   = false;   // writers provably span >=2 thread roles
    uint64_t slotsPerLine = 0;
    uint64_t contendedLines = 0;
    StripeMitigation mitigation = StripeMitigation::None;

    WriteFrequencyTier frequency = WriteFrequencyTier::Unknown;
    uint64_t currentFootprint = 0;
    uint64_t paddedFootprint  = 0;
    double   l1dCostFraction  = 0.0;   // added bytes / L1D
    StripeFixShape fixShape   = StripeFixShape::None;
    std::string fixRationale;
};

StripeVerdict gradeStripedArray(const StripedArraySite &s,
                                const ThreadRoleVerdicts &roles,
                                uint64_t lineBytes);

// Full padding pays only against frequency. Above this share of L1D it
// evicts the working set it was meant to protect.
inline constexpr double kFullPadL1DBudget = 0.10;

void applyStripeROI(StripeVerdict &v, const StripedArraySite &s,
                    uint64_t lineBytes, uint64_t l1dSizeBytes,
                    bool alignedOwnerAvailable);

} // namespace lshaz
