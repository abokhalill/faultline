#pragma once

#include "faultline/core/Diagnostic.h"
#include "faultline/core/HotPathOracle.h"

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/ASTContext.h>

#include <vector>

namespace faultline {

// Walks the AST collecting struct/class record declarations for layout analysis.
// Individual rules are invoked per-declaration from the consumer.
class StructLayoutVisitor
    : public clang::RecursiveASTVisitor<StructLayoutVisitor> {
public:
    explicit StructLayoutVisitor(clang::ASTContext &Ctx);

    bool VisitCXXRecordDecl(clang::CXXRecordDecl *RD);

    const std::vector<clang::CXXRecordDecl *> &records() const {
        return records_;
    }

private:
    clang::ASTContext &ctx_;
    std::vector<clang::CXXRecordDecl *> records_;
};

} // namespace faultline
