// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lshaz {

// Per-TU thread-attribution facts, keyed by function/field name so they
// join across TUs (a pthread_create call and its entry's definition are
// usually in different TUs).
//
// Ordered containers throughout: IPC serialization and reduce iteration
// must not depend on hash-table layout.
struct ThreadRoleSummary {
    // Functions observed passed to a thread-creation primitive
    // (pthread_create, thrd_create, std::thread/jthread, std::async).
    std::set<std::string> threadEntries;

    // Direct call edges among user functions: caller -> callees.
    std::map<std::string, std::set<std::string>> callEdges;

    // "type_name::field_name" -> functions writing that field in this TU.
    std::map<std::string, std::set<std::string>> fieldWriters;

    // Loop nesting at each call site, and each function's own maximum loop
    // depth. Hotness inference is loop-depth-weighted, so the reduce phase
    // needs both to rerun the per-TU relaxation over the merged graph rather
    // than degrading every cross-TU callee to the weakest grade.
    std::map<std::string, std::map<std::string, unsigned>> edgeLoopDepth;
    std::map<std::string, unsigned> ownLoopDepth;

    // Functions that allocate, and functions that free, a block of a given
    // pointee type. The join key is the type name because it is the only
    // thing that crosses a TU boundary: the allocation and the free that
    // releases it routinely sit in different files, and no pointer value
    // survives the split. A site whose type cannot be named is left out,
    // which leaves FL020's conjunct unestablished rather than guessed.
    std::map<std::string, std::set<std::string>> allocatorsOfType;
    std::map<std::string, std::set<std::string>> freersOfType;

    // Raw structure for inferring the project's own allocator vocabulary,
    // rather than being told it. Requiring a human to declare zmalloc,
    // ngx_palloc, palloc, xmalloc or kmalloc before the rule can see anything
    // is hardcoding with a config file in front of it, and it has to be
    // rediscovered per codebase.
    //
    // returnForwards: F returns the result of calling G, so F allocates if G
    // does. paramForwards: F hands one of its own parameters to G, so F frees
    // if G does. Seeded from the libc primitives, which are ABI rather than
    // vocabulary, and closed over the merged graph in the reduce phase.
    std::map<std::string, std::set<std::string>> returnForwards;
    std::map<std::string, std::set<std::string>> paramForwards;

    // Call sites keyed by callee: "F|T" meaning that inside F, the callee
    // produced (or was handed) a T*. Attribution waits for the reduce phase,
    // which is the first point that knows whether the callee allocates.
    std::map<std::string, std::set<std::string>> allocSitesByCallee;
    std::map<std::string, std::set<std::string>> freeSitesByCallee;

    // Seeds taken from the declaration rather than its spelling. A libc name
    // list is defeated by one #define: redis builds jemalloc with
    // "#define malloc(size) je_malloc(size)", so no seed name survives
    // preprocessing anywhere in the tree. The attributes do survive, and an
    // allocator carries them because the optimizer needs them, which is what
    // makes this a seed rule no name list has to keep up with.
    //
    // Produced only by the vocabulary prepass, which runs in the parent, so
    // these never cross the IPC boundary.
    std::set<std::string> declaredAllocators;
    std::set<std::string> declaredFreers;

    // Virtual methods some class actually overrides, qualified names. A call
    // to a method absent here is monomorphic program-wide, which is the
    // difference between paying ~1ns and ~9ns. Only the merged set can say,
    // the override usually lives in another TU than the call.
    std::set<std::string> overriddenVirtuals;

    void merge(const ThreadRoleSummary &other) {
        threadEntries.insert(other.threadEntries.begin(),
                             other.threadEntries.end());
        for (const auto &[caller, callees] : other.callEdges)
            callEdges[caller].insert(callees.begin(), callees.end());
        for (const auto &[field, writers] : other.fieldWriters)
            fieldWriters[field].insert(writers.begin(), writers.end());
        // Max, not overwrite: an inline body seen in several TUs must not
        // depend on which shard reported it last, or output stops being
        // jobs-invariant.
        for (const auto &[caller, edges] : other.edgeLoopDepth) {
            auto &dst = edgeLoopDepth[caller];
            for (const auto &[callee, d] : edges) {
                auto &cur = dst[callee];
                if (d > cur) cur = d;
            }
        }
        for (const auto &[fn, d] : other.ownLoopDepth) {
            auto &cur = ownLoopDepth[fn];
            if (d > cur) cur = d;
        }
        for (const auto &[ty, fns] : other.allocatorsOfType)
            allocatorsOfType[ty].insert(fns.begin(), fns.end());
        for (const auto &[ty, fns] : other.freersOfType)
            freersOfType[ty].insert(fns.begin(), fns.end());
        for (const auto &[f, gs] : other.returnForwards)
            returnForwards[f].insert(gs.begin(), gs.end());
        for (const auto &[f, gs] : other.paramForwards)
            paramForwards[f].insert(gs.begin(), gs.end());
        for (const auto &[g, sites] : other.allocSitesByCallee)
            allocSitesByCallee[g].insert(sites.begin(), sites.end());
        for (const auto &[g, sites] : other.freeSitesByCallee)
            freeSitesByCallee[g].insert(sites.begin(), sites.end());
        declaredAllocators.insert(other.declaredAllocators.begin(),
                                  other.declaredAllocators.end());
        declaredFreers.insert(other.declaredFreers.begin(),
                              other.declaredFreers.end());
        overriddenVirtuals.insert(other.overriddenVirtuals.begin(),
                                  other.overriddenVirtuals.end());
    }

    bool empty() const {
        return threadEntries.empty() && callEdges.empty() &&
               fieldWriters.empty();
    }
};

// Role bitmask. A function reachable from both roots is MAIN|WORKER and
// its writes attribute to both; the conservative direction: escalation
// requires provably disjoint masks.
enum ThreadRoleMask : uint8_t {
    ROLE_NONE   = 0,
    ROLE_MAIN   = 1,
    ROLE_WORKER = 2,
};

// Reduce-phase verdicts over the merged summary.
struct ThreadRoleVerdicts {
    // Only functions with a known role appear; absence means unknown.
    std::map<std::string, uint8_t> functionRoles;

    uint8_t roleOf(const std::string &fn) const {
        auto it = functionRoles.find(fn);
        return it != functionRoles.end() ? it->second : ROLE_NONE;
    }

    // Union of writer roles for a field. ROLE_NONE if any writer is
    // unknown; a partial attribution cannot prove disjointness.
    uint8_t fieldWriterRoles(const ThreadRoleSummary &facts,
                             const std::string &fieldKey) const {
        auto it = facts.fieldWriters.find(fieldKey);
        if (it == facts.fieldWriters.end() || it->second.empty())
            return ROLE_NONE;
        uint8_t mask = 0;
        for (const auto &w : it->second) {
            uint8_t r = roleOf(w);
            if (r == ROLE_NONE)
                return ROLE_NONE;
            mask |= r;
        }
        return mask;
    }

    // Union of roles over a named function set. ROLE_NONE if any member is
    // unattributed, since a partial answer cannot prove disjointness.
    uint8_t rolesOf(const std::set<std::string> &fns) const {
        if (fns.empty())
            return ROLE_NONE;
        uint8_t mask = 0;
        for (const auto &f : fns) {
            uint8_t r = roleOf(f);
            if (r == ROLE_NONE)
                return ROLE_NONE;
            mask |= r;
        }
        return mask;
    }

    // Every allocation of this type on one thread role, every free on the
    // other. The measured 25x is a property of that split, not of allocation
    // volume, so this is what FL020's conjunct gates on.
    bool typeIsFreedCrossThread(const ThreadRoleSummary &facts,
                                const std::string &typeName) const {
        auto a = facts.allocatorsOfType.find(typeName);
        auto f = facts.freersOfType.find(typeName);
        if (a == facts.allocatorsOfType.end() ||
            f == facts.freersOfType.end())
            return false;
        uint8_t am = rolesOf(a->second), fm = rolesOf(f->second);
        return am != ROLE_NONE && fm != ROLE_NONE && (am & fm) == 0;
    }

    // True when both fields have fully-attributed writers and the role
    // sets are disjoint and non-empty: every writer of A on one thread
    // role, every writer of B on the other.
    bool fieldsHaveDisjointWriterRoles(const ThreadRoleSummary &facts,
                                       const std::string &fieldA,
                                       const std::string &fieldB) const {
        uint8_t a = fieldWriterRoles(facts, fieldA);
        uint8_t b = fieldWriterRoles(facts, fieldB);
        return a != ROLE_NONE && b != ROLE_NONE && (a & b) == 0;
    }
};

// Closes the allocator and freer sets over the merged graph and turns the
// recorded call sites into allocatorsOfType / freersOfType. Seeds are the libc
// primitives only, so a project's own names are derived rather than declared.
// extraAlloc / extraFree add configured patterns for the cases structure
// cannot reach: an allocator whose result leaves through an out-parameter, or
// one whose body this scan never saw.
//
// Mutates the two type maps in place; pure in its other inputs.
void inferAllocatorVocabulary(ThreadRoleSummary &facts,
                              const std::vector<std::string> &extraAlloc,
                              std::set<std::string> &allocatorsOut,
                              std::set<std::string> &freersOut);

// BFS role propagation over the merged call graph. Roots: "main" (plus
// mainPatterns matches) seed ROLE_MAIN; threadEntries (plus entryPatterns
// matches, fnmatch globs against every known function name) seed
// ROLE_WORKER. Pure function of its inputs.
//
// Known limitation: Function-pointer dispatch (event-loop handler tables) breaks
// the chain; entryPatterns exist so codebases like that can name their
// worker roots explicitly in config.
ThreadRoleVerdicts computeThreadRoles(
    const ThreadRoleSummary &facts,
    const std::vector<std::string> &entryPatterns,
    const std::vector<std::string> &mainPatterns);

} // namespace lshaz
