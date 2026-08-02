// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace lshaz {

// Per-type escape signals collected from a single TU.
// Structural signals (atomics, sync, volatile, shared_ptr)
// are derivable from the type definition alone; they're identical across
// all TUs that include the header. Publication signals (global store,
// thread-creation arg) are TU-specific and must be aggregated.
struct TypeEscapeSignals {
    bool hasAtomics      = false;
    bool hasSyncPrims    = false;
    bool hasSharedOwner  = false;
    bool hasVolatile     = false;
    bool hasPublication  = false;  // TU-local: passed to thread/stored globally
    // Written from >=2 functions, one of them spawned as a thread.
    // Publication requires an address to cross a thread boundary; a
    // file-scope object written directly from two thread bodies never
    // does, and is invisible without this.
    bool hasThreadWriters = false;
    // A file-scope instance exists in SOME TU. Per-TU this is nearly
    // useless -- the record lives in a header and the global lives in one
    // .c -- which is why the instance question has to be answered here
    // rather than at rule time.
    bool hasGlobalInstance = false;
    // Some TU saw a thread-borne writer touch it.
    bool hasThreadBorneWriter = false;
    // Some TU writes a field through a fixed nameable object rather than
    // one passed in. A handed-over object has one owner at a time.
    bool hasStandingWrites = false;
    // Explicit line alignment or trailing pad-to-line: the author reasons
    // in cache lines. Feeds the FL092 precedent join; a codebase-level
    // "the mitigation idiom is known here" index.
    bool hasDeliberateLayout = false;
    unsigned accessorCount = 0;   // distinct functions touching this type in TU

    // Merge another TU's signals into this aggregate.
    void merge(const TypeEscapeSignals &other) {
        hasAtomics     |= other.hasAtomics;
        hasSyncPrims   |= other.hasSyncPrims;
        hasSharedOwner |= other.hasSharedOwner;
        hasVolatile    |= other.hasVolatile;
        hasPublication |= other.hasPublication;
        hasThreadWriters |= other.hasThreadWriters;
        hasGlobalInstance |= other.hasGlobalInstance;
        hasThreadBorneWriter |= other.hasThreadBorneWriter;
        hasStandingWrites |= other.hasStandingWrites;
        hasDeliberateLayout |= other.hasDeliberateLayout;
        accessorCount  += other.accessorCount;
    }

    // Structural signals only — no TU-specific publication evidence.
    bool hasStructuralEscape() const {
        return hasAtomics || hasSyncPrims || hasSharedOwner || hasVolatile;
    }

    // Two cores can reach one object: a shared instance somewhere, and a
    // thread that touches it. Answerable only after every TU has reported.
    bool hasSharingRoute() const {
        // Standing access is the conjunct that separates sharing from a
        // handoff. Without it, a request object queued between a producer
        // and a consumer looks identical to a shared counter block: both
        // are written from several functions, one of them thread-borne.
        if (!hasStandingWrites)
            return false;
        return hasPublication || hasThreadWriters ||
               (hasGlobalInstance && hasThreadBorneWriter);
    }

    bool hasAnyEscape() const {
        // hasGlobalInstance belongs here. Splitting it out of hasPublication
        // and forgetting this suppressed every type whose only evidence is
        // that a global instance exists -- which is most file-local hot
        // structs in C. memcached's itemstats, 27k contended writes measured,
        // fired when items.c was scanned alone and vanished in the corpus run.
        return hasStructuralEscape() || hasPublication || hasThreadWriters ||
               hasGlobalInstance;
    }
};

// Per-TU escape summary. Keyed by canonical qualified type name.
// Emitted by LshazASTConsumer, serialized through fork IPC,
// aggregated in ScanPipeline reduce phase.
using EscapeSummary = std::unordered_map<std::string, TypeEscapeSignals>;

// Merge src into dst. For each type, merge signals.
inline void mergeEscapeSummaries(EscapeSummary &dst, const EscapeSummary &src) {
    for (const auto &[name, signals] : src)
        dst[name].merge(signals);
}

} // namespace lshaz
