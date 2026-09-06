// SPDX-License-Identifier: Apache-2.0
#ifndef LSHAZ_ANALYSIS_LOOPSHAPE_H
#define LSHAZ_ANALYSIS_LOOPSHAPE_H

#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>

#include <llvm/Support/Casting.h>

namespace lshaz {

// A loop whose controlling expression folds to zero does not repeat: the
// do/while(0) body runs exactly once, the while(0) body never.
inline const clang::Expr *loopCondition(const clang::Stmt *S) {
    if (const auto *D = llvm::dyn_cast_or_null<clang::DoStmt>(S))
        return D->getCond();
    if (const auto *W = llvm::dyn_cast_or_null<clang::WhileStmt>(S))
        return W->getCond();
    if (const auto *F = llvm::dyn_cast_or_null<clang::ForStmt>(S))
        return F->getCond();
    return nullptr;
}

inline bool conditionFoldsToZero(const clang::Expr *cond,
                                 const clang::ASTContext &Ctx) {
    if (!cond) return false;
    if (auto v = cond->getIntegerConstantExpr(Ctx))
        return v->isZero();
    return false;
}

// True when the statement is a loop in syntax only. Callers that count
// loop depth should not count these.
inline bool isDegenerateLoop(const clang::Stmt *S,
                             const clang::ASTContext &Ctx) {
    return conditionFoldsToZero(loopCondition(S), Ctx);
}

} // namespace lshaz

#endif // LSHAZ_ANALYSIS_LOOPSHAPE_H
