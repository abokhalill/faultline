#ifndef LSHAZ_ANALYSIS_LAYOUTSAFETY_H
#define LSHAZ_ANALYSIS_LAYOUTSAFETY_H

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Type.h>

namespace lshaz {

/// Returns true if Clang can safely compute the layout/size of the given
/// QualType without crashing.  Checks for dependent types, incomplete types,
/// and records whose fields recursively contain non-layoutable types.
inline bool canComputeTypeSize(clang::QualType QT, clang::ASTContext &Ctx) {
    if (QT.isNull())
        return false;
    QT = QT.getCanonicalType();
    if (QT->isDependentType() || QT->isIncompleteType())
        return false;
    if (QT->containsErrors())
        return false;
    if (QT->isUndeducedAutoType())
        return false;
    if (QT->isSizelessType())
        return false;
    if (QT->containsUnexpandedParameterPack())
        return false;
    // Unresolved 'using' types.
    if (QT->getAs<clang::UnresolvedUsingType>())
        return false;
    // Array element type must also be layoutable.
    if (const auto *ArrT = Ctx.getAsArrayType(QT))
        return canComputeTypeSize(ArrT->getElementType(), Ctx);
    // For record types, check the record itself.
    if (const auto *RT = QT->getAs<clang::RecordType>()) {
        const auto *RD = RT->getDecl();
        if (!RD || RD->isInvalidDecl() || !RD->isCompleteDefinition())
            return false;
        if (RD->isDependentType())
            return false;
        // Check all fields recursively.
        for (const auto *field : RD->fields()) {
            if (!canComputeTypeSize(field->getType(), Ctx))
                return false;
        }
        // Check bases for CXXRecordDecl.
        if (const auto *CXXRD = llvm::dyn_cast<clang::CXXRecordDecl>(RD)) {
            for (const auto &base : CXXRD->bases()) {
                if (!canComputeTypeSize(base.getType(), Ctx))
                    return false;
            }
            for (const auto &vbase : CXXRD->vbases()) {
                if (!canComputeTypeSize(vbase.getType(), Ctx))
                    return false;
            }
        }
    }
    return true;
}

/// Returns true if Clang can safely call getASTRecordLayout on this record.
inline bool canComputeRecordLayout(const clang::CXXRecordDecl *RD,
                                   clang::ASTContext &Ctx) {
    if (!RD || !RD->isCompleteDefinition())
        return false;
    if (RD->isDependentType() || RD->isInvalidDecl())
        return false;
    return canComputeTypeSize(Ctx.getRecordType(RD), Ctx);
}

} // namespace lshaz

#endif // LSHAZ_ANALYSIS_LAYOUTSAFETY_H
