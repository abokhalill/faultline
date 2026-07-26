// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/analysis/ThreadRoleSummary.h"

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

struct StripeVerdict {
    uint8_t  writerRoles = ROLE_NONE;
    unsigned writerCount = 0;
    bool     multiRole   = false;   // writers provably span >=2 thread roles
    uint64_t slotsPerLine = 0;
    uint64_t contendedLines = 0;
    StripeMitigation mitigation = StripeMitigation::None;
};

StripeVerdict gradeStripedArray(const StripedArraySite &s,
                                const ThreadRoleVerdicts &roles,
                                uint64_t lineBytes);

} // namespace lshaz
