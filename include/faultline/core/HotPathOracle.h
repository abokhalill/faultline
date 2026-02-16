#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace clang {
class Decl;
class FunctionDecl;
class Attr;
} // namespace clang

namespace faultline {

struct Config;

// Determines whether a given declaration resides on a hot path.
// Three mechanisms per ARCHITECTURE.md §3:
//   1. [[clang::annotate("faultline_hot")]] attribute on functions
//   2. Config-based function/file pattern matching
//   3. Heuristic: callee of annotated entry points (Phase 1+)
class HotPathOracle {
public:
    explicit HotPathOracle(const Config &cfg);

    bool isHot(const clang::Decl *D) const;
    bool isFunctionHot(const clang::FunctionDecl *FD) const;

    // Manually mark a function as hot (used during AST walk).
    void markHot(const clang::FunctionDecl *FD);

private:
    bool hasHotAnnotation(const clang::FunctionDecl *FD) const;
    bool matchesConfigPattern(const clang::FunctionDecl *FD) const;

    const Config &config_;
    mutable std::unordered_set<const clang::FunctionDecl *> hotCache_;
};

} // namespace faultline
