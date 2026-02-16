#include "faultline/analysis/StructLayoutVisitor.h"

#include <clang/AST/DeclCXX.h>

namespace faultline {

StructLayoutVisitor::StructLayoutVisitor(clang::ASTContext &Ctx) : ctx_(Ctx) {}

bool StructLayoutVisitor::VisitCXXRecordDecl(clang::CXXRecordDecl *RD) {
    if (!RD->isCompleteDefinition())
        return true;

    if (RD->isImplicit() || RD->isLambda())
        return true;

    records_.push_back(RD);
    return true;
}

} // namespace faultline
