#pragma once

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Type.h>

namespace faultline {

// Heuristic thread-escape analysis.
// Conservative: if uncertain, assumes escape (per ROADMAP §3.5).
//
// A type is considered thread-escaping if any of:
//   1. It has std::atomic member fields
//   2. It is passed to a function taking std::thread, std::async, etc.
//   3. It is stored in a global/static mutable variable
//   4. It contains a std::mutex or similar synchronization primitive
//   5. It is used as a template argument to std::shared_ptr
//
// Phase 1 implements checks 1, 3, 4 only. Interprocedural analysis is Phase 2+.
class EscapeAnalysis {
public:
    explicit EscapeAnalysis(clang::ASTContext &Ctx);

    // Does this record type contain evidence of cross-thread usage?
    bool mayEscapeThread(const clang::CXXRecordDecl *RD) const;

    // Does this specific field suggest shared-write access?
    bool isFieldMutable(const clang::FieldDecl *FD) const;

    // Does the type contain atomic members?
    bool hasAtomicMembers(const clang::CXXRecordDecl *RD) const;

    // Does the type contain synchronization primitives?
    bool hasSyncPrimitives(const clang::CXXRecordDecl *RD) const;

    // Is this a global/static with mutable state?
    bool isGlobalSharedMutable(const clang::VarDecl *VD) const;

    bool isAtomicType(clang::QualType QT) const;
    bool isSyncType(clang::QualType QT) const;

private:

    clang::ASTContext &ctx_;
};

} // namespace faultline
