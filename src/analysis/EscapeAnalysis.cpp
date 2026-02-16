#include "faultline/analysis/EscapeAnalysis.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Type.h>

namespace faultline {

EscapeAnalysis::EscapeAnalysis(clang::ASTContext &Ctx) : ctx_(Ctx) {}

bool EscapeAnalysis::isAtomicType(clang::QualType QT) const {
    QT = QT.getCanonicalType();
    if (QT.getAsString().find("atomic") != std::string::npos)
        return true;
    if (QT->isAtomicType())
        return true;
    return false;
}

bool EscapeAnalysis::isSyncType(clang::QualType QT) const {
    std::string name = QT.getCanonicalType().getAsString();
    // std::mutex, std::recursive_mutex, std::shared_mutex, std::timed_mutex
    if (name.find("mutex") != std::string::npos)
        return true;
    // std::condition_variable
    if (name.find("condition_variable") != std::string::npos)
        return true;
    // pthread_mutex_t, pthread_spinlock_t
    if (name.find("pthread_mutex") != std::string::npos ||
        name.find("pthread_spinlock") != std::string::npos)
        return true;
    return false;
}

bool EscapeAnalysis::hasAtomicMembers(const clang::CXXRecordDecl *RD) const {
    if (!RD || !RD->isCompleteDefinition())
        return false;

    for (const auto *field : RD->fields()) {
        if (isAtomicType(field->getType()))
            return true;
    }

    // Check bases.
    for (const auto &base : RD->bases()) {
        if (const auto *baseRD = base.getType()->getAsCXXRecordDecl()) {
            if (hasAtomicMembers(baseRD))
                return true;
        }
    }

    return false;
}

bool EscapeAnalysis::hasSyncPrimitives(const clang::CXXRecordDecl *RD) const {
    if (!RD || !RD->isCompleteDefinition())
        return false;

    for (const auto *field : RD->fields()) {
        if (isSyncType(field->getType()))
            return true;
    }

    for (const auto &base : RD->bases()) {
        if (const auto *baseRD = base.getType()->getAsCXXRecordDecl()) {
            if (hasSyncPrimitives(baseRD))
                return true;
        }
    }

    return false;
}

bool EscapeAnalysis::mayEscapeThread(const clang::CXXRecordDecl *RD) const {
    if (!RD)
        return false;

    if (hasAtomicMembers(RD))
        return true;
    if (hasSyncPrimitives(RD))
        return true;
    if (hasSharedOwnershipMembers(RD))
        return true;

    return false;
}

bool EscapeAnalysis::isFieldMutable(const clang::FieldDecl *FD) const {
    if (!FD)
        return false;

    // Explicitly mutable keyword.
    if (FD->isMutable())
        return true;

    // Non-const qualified type.
    if (!FD->getType().isConstQualified())
        return true;

    return false;
}

bool EscapeAnalysis::isGlobalSharedMutable(const clang::VarDecl *VD) const {
    if (!VD)
        return false;

    // Must be global or static.
    if (!VD->hasGlobalStorage())
        return false;

    // Must not be const.
    if (VD->getType().isConstQualified())
        return false;

    // thread_local is not shared.
    if (VD->getTSCSpec() == clang::ThreadStorageClassSpecifier::TSCS_thread_local)
        return false;

    return true;
}

bool EscapeAnalysis::isSharedOwnershipType(clang::QualType QT) const {
    std::string name = QT.getCanonicalType().getAsString();
    if (name.find("shared_ptr") != std::string::npos)
        return true;
    if (name.find("weak_ptr") != std::string::npos)
        return true;
    return false;
}

bool EscapeAnalysis::hasSharedOwnershipMembers(const clang::CXXRecordDecl *RD) const {
    if (!RD || !RD->isCompleteDefinition())
        return false;

    for (const auto *field : RD->fields()) {
        if (isSharedOwnershipType(field->getType()))
            return true;
    }

    for (const auto &base : RD->bases()) {
        if (const auto *baseRD = base.getType()->getAsCXXRecordDecl()) {
            if (hasSharedOwnershipMembers(baseRD))
                return true;
        }
    }

    return false;
}

bool EscapeAnalysis::hasCallbackMembers(const clang::CXXRecordDecl *RD) const {
    if (!RD || !RD->isCompleteDefinition())
        return false;

    for (const auto *field : RD->fields()) {
        std::string typeName = field->getType().getCanonicalType().getAsString();
        if (typeName.find("std::function") != std::string::npos)
            return true;
        if (field->getType()->isFunctionPointerType())
            return true;
    }

    return false;
}

} // namespace faultline
