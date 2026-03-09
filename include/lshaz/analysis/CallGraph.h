// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lshaz {

// Lightweight per-TU call graph built from the Clang AST.
// Maps caller -> set of direct callees (resolved FunctionDecl pointers).
// Used for hot-path transitivity: if f() is hot and calls g(), g() is hot.
class CallGraph {
public:
    explicit CallGraph(clang::ASTContext &Ctx) : ctx_(Ctx) {}

    // Build the call graph from all function bodies in the TU.
    void buildFromTU(const clang::TranslationUnitDecl *TU);

    // Get direct callees of a function.
    const std::unordered_set<const clang::FunctionDecl *> &
    callees(const clang::FunctionDecl *Caller) const;

    // Get direct callers of a function.
    const std::unordered_set<const clang::FunctionDecl *> &
    callers(const clang::FunctionDecl *Callee) const;

    // Compute transitive callees from a set of root functions up to maxDepth.
    // Returns all functions reachable within maxDepth call edges.
    std::unordered_set<const clang::FunctionDecl *>
    transitiveCallees(
        const std::unordered_set<const clang::FunctionDecl *> &roots,
        unsigned maxDepth = 8) const;

    size_t numFunctions() const { return calleeMap_.size(); }
    size_t numEdges() const { return edgeCount_; }

private:
    void processFunction(const clang::FunctionDecl *FD);

    clang::ASTContext &ctx_;

    // caller -> callees
    std::unordered_map<const clang::FunctionDecl *,
                       std::unordered_set<const clang::FunctionDecl *>>
        calleeMap_;

    // callee -> callers (reverse edges)
    std::unordered_map<const clang::FunctionDecl *,
                       std::unordered_set<const clang::FunctionDecl *>>
        callerMap_;

    size_t edgeCount_ = 0;

    static const std::unordered_set<const clang::FunctionDecl *> empty_;
};

} // namespace lshaz
