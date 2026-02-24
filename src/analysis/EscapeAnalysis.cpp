#include "faultline/analysis/EscapeAnalysis.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>

namespace faultline {

EscapeAnalysis::EscapeAnalysis(clang::ASTContext &Ctx) : ctx_(Ctx) {}

namespace {

// Resolve through typedefs/aliases to the underlying CXXRecordDecl.
const clang::CXXRecordDecl *getUnderlyingRecord(clang::QualType QT) {
    QT = QT.getCanonicalType();
    QT = QT.getNonReferenceType();
    if (const auto *TST = QT->getAs<clang::TemplateSpecializationType>()) {
        if (auto TD = TST->getTemplateName().getAsTemplateDecl()) {
            if (auto *RD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
                    TD->getTemplatedDecl()))
                return RD;
        }
    }
    return QT->getAsCXXRecordDecl();
}

bool isQualifiedNameOneOf(const clang::CXXRecordDecl *RD,
                          const std::initializer_list<const char *> &names) {
    if (!RD)
        return false;
    std::string qn = RD->getQualifiedNameAsString();
    for (const char *n : names)
        if (qn == n)
            return true;
    return false;
}

} // anonymous namespace

bool EscapeAnalysis::isAtomicType(clang::QualType QT) const {
    // C11 _Atomic qualifier.
    if (QT.getCanonicalType()->isAtomicType())
        return true;

    // std::atomic<T> — match via template specialization.
    const clang::CXXRecordDecl *RD = getUnderlyingRecord(QT);
    if (isQualifiedNameOneOf(RD, {"std::atomic", "std::atomic_ref"}))
        return true;

    // ClassTemplateSpecializationDecl path for instantiated types.
    if (RD) {
        if (const auto *CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(RD)) {
            if (auto *TD = CTSD->getSpecializedTemplate()) {
                std::string tn = TD->getQualifiedNameAsString();
                if (tn == "std::atomic" || tn == "std::atomic_ref")
                    return true;
            }
        }
    }

    return false;
}

bool EscapeAnalysis::isSyncType(clang::QualType QT) const {
    const clang::CXXRecordDecl *RD = getUnderlyingRecord(QT);
    if (isQualifiedNameOneOf(RD, {
            "std::mutex", "std::recursive_mutex",
            "std::shared_mutex", "std::timed_mutex",
            "std::recursive_timed_mutex", "std::shared_timed_mutex",
            "std::condition_variable", "std::condition_variable_any",
            "std::counting_semaphore", "std::binary_semaphore",
            "std::latch", "std::barrier"}))
        return true;

    // POSIX sync types (C structs, no CXXRecordDecl).
    std::string canon = QT.getCanonicalType().getAsString();
    for (const char *posix : {"pthread_mutex_t", "pthread_spinlock_t",
                              "pthread_rwlock_t", "pthread_cond_t",
                              "sem_t"})
        if (canon.find(posix) != std::string::npos)
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
    if (hasVolatileMembers(RD))
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
    const clang::CXXRecordDecl *RD = getUnderlyingRecord(QT);
    if (isQualifiedNameOneOf(RD, {"std::shared_ptr", "std::weak_ptr"}))
        return true;

    if (RD) {
        if (const auto *CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(RD)) {
            if (auto *TD = CTSD->getSpecializedTemplate()) {
                std::string tn = TD->getQualifiedNameAsString();
                if (tn == "std::shared_ptr" || tn == "std::weak_ptr")
                    return true;
            }
        }
    }

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
        if (field->getType()->isFunctionPointerType())
            return true;

        const clang::CXXRecordDecl *FRD = getUnderlyingRecord(field->getType());
        if (isQualifiedNameOneOf(FRD, {"std::function"}))
            return true;
        if (FRD) {
            if (const auto *CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(FRD)) {
                if (auto *TD = CTSD->getSpecializedTemplate()) {
                    if (TD->getQualifiedNameAsString() == "std::function")
                        return true;
                }
            }
        }
    }

    return false;
}

bool EscapeAnalysis::hasVolatileMembers(const clang::CXXRecordDecl *RD) const {
    if (!RD || !RD->isCompleteDefinition())
        return false;

    for (const auto *field : RD->fields()) {
        if (field->getType().isVolatileQualified())
            return true;
    }

    for (const auto &base : RD->bases()) {
        if (const auto *baseRD = base.getType()->getAsCXXRecordDecl()) {
            if (hasVolatileMembers(baseRD))
                return true;
        }
    }

    return false;
}

} // namespace faultline
