// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lshaz/core/Severity.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace clang {
class Decl;
class FunctionDecl;
class Attr;
} // namespace clang

namespace lshaz {

class CallGraph;
struct Config;

// How a function came to be considered hot. Ordered weakest to strongest:
// an operator's explicit assertion and a real profile are evidence, a
// structural inference is a hypothesis the shape of the code supports.
// Rules must not treat these alike — see supportedCeiling().
enum class HotnessSource : uint8_t {
    None = 0,
    InferredShallow,  // reached from an entry through exactly one loop level
    InferredDeep,     // nested loops, or recursion (self-repetition)
    Declared,         // __attribute__((hot)) / annotate / config pattern
    Profiled,         // named by --perf-profile
};

// Determines whether a given declaration resides on a hot path.
//   1. [[clang::annotate("lshaz_hot")]] or __attribute__((hot))
//   2. Config-based function/file pattern matching
//   3. Perf/LBR profile: function name exceeds sample threshold
//   4. Manual markHot() calls during AST walk
//   5. Structural inference: loop-depth-weighted reachability from the
//      TU's entry points, which needs no per-project configuration
class HotPathOracle {
public:
    explicit HotPathOracle(const Config &cfg);

    bool isHot(const clang::Decl *D) const;
    bool isFunctionHot(const clang::FunctionDecl *FD) const;

    // Why this function is hot. None when it is not.
    HotnessSource hotnessSource(const clang::FunctionDecl *FD) const;

    void markHot(const clang::FunctionDecl *FD);

    // Propagate hotness transitively through a call graph.
    // All functions reachable from currently-hot roots within maxDepth
    // call edges are marked hot.
    void propagateViaCallGraph(const CallGraph &cg, unsigned maxDepth = 8);

    // Derive hotness from code shape when nothing else supplies it.
    //
    // Repetition is what turns a cache miss into a steady-state cost, and a
    // loop is where repetition is written down. Seeded from the TU's thread
    // entries and main, a callee inherits its caller's hotness plus the loop
    // nesting at the call site; recursion is self-repetition and counts.
    // Nothing here names a project symbol, so it cannot overfit to one
    // codebase — but it is per-TU, so a function looped over from another
    // translation unit is invisible to it.
    void inferFromCodeShape(const CallGraph &cg);

    // Load profile-derived hot function names (demangled qualified names).
    void loadProfileHotFunctions(std::unordered_set<std::string> names);

private:
    bool hasHotAnnotation(const clang::FunctionDecl *FD) const;
    bool matchesConfigPattern(const clang::FunctionDecl *FD) const;
    bool matchesProfileFunction(const clang::FunctionDecl *FD) const;
    void record(const clang::FunctionDecl *FD, HotnessSource src) const;

    const Config &config_;
    mutable std::unordered_set<const clang::FunctionDecl *> hotCache_;
    mutable std::unordered_map<const clang::FunctionDecl *, HotnessSource>
        sources_;
    std::unordered_set<std::string> profileHotFunctions_;
};

const char *hotnessSourceName(HotnessSource s);

// A hot-path rule's mechanism claim rests on the code actually running often.
// An inference establishes shape, not execution: it justifies looking, not a
// grade that implies measured cost. Rules pass their own base severity and
// get back what the evidence for hotness can carry.
Severity hotnessSupportedSeverity(HotnessSource s, Severity base);

} // namespace lshaz
