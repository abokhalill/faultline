// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/vocabulary.h"
#include "lshaz/analysis/symbols.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>

#include <map>

namespace lshaz {

namespace {

// Nothing here matches a name. The reduce phase closes the allocator and freer
// sets over these edges from the libc seeds, which is what lets zmalloc,
// ngx_palloc, palloc and kmalloc be discovered rather than declared.
//
// Sites whose pointee type cannot be named are dropped, so FL020's conjunct
// fails closed rather than guessing.
class AllocOwnershipVisitor
    : public clang::RecursiveASTVisitor<AllocOwnershipVisitor> {
public:
    AllocOwnershipVisitor(clang::ASTContext &C, ThreadRoleSummary &out)
        : ctx_(C), out_(out) {}

    bool TraverseFunctionDecl(clang::FunctionDecl *FD) {
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return true;
        const auto *savedFn = fn_;
        auto savedVars = std::move(varSource_);
        auto savedTaint = std::move(paramTainted_);
        varSource_.clear();
        paramTainted_.clear();
        fn_ = FD;
        out_.definedFunctions.insert(threadRoleNodeName(FD, ctx_));
        bool r = clang::RecursiveASTVisitor<
            AllocOwnershipVisitor>::TraverseFunctionDecl(FD);
        fn_ = savedFn;
        varSource_ = std::move(savedVars);
        paramTainted_ = std::move(savedTaint);
        return r;
    }
    bool TraverseCXXMethodDecl(clang::CXXMethodDecl *MD) {
        return TraverseFunctionDecl(MD);
    }

    // new/delete need no inference: the operator is the allocator.
    bool VisitCXXNewExpr(clang::CXXNewExpr *E) {
        if (fn_)
            noteType(out_.allocatorsOfType, E->getAllocatedType());
        return true;
    }
    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *E) {
        if (fn_ && E->getArgument())
            noteType(out_.freersOfType, pointee(E->getArgument()->getType()));
        return true;
    }

    // T *p = G(...)
    bool VisitVarDecl(clang::VarDecl *VD) {
        if (!fn_ || !VD || !VD->hasInit())
            return true;
        if (mentionsParam(VD->getInit()))
            paramTainted_.insert(VD->getCanonicalDecl());
        std::string g = calleeOf(VD->getInit());
        if (g.empty())
            return true;
        varSource_[VD->getCanonicalDecl()].insert(g);
        noteSite(out_.allocSitesByCallee, g, pointee(VD->getType()));
        return true;
    }

    // p = G(...). Outnumbers the declaration form in real C.
    bool VisitBinaryOperator(clang::BinaryOperator *BO) {
        if (!fn_ || !BO || BO->getOpcode() != clang::BO_Assign)
            return true;
        if (mentionsParam(BO->getRHS()))
            if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(
                    BO->getLHS()->IgnoreParenImpCasts()))
                if (const auto *VD =
                        llvm::dyn_cast<clang::VarDecl>(DRE->getDecl()))
                    paramTainted_.insert(VD->getCanonicalDecl());
        std::string g = calleeOf(BO->getRHS());
        if (g.empty())
            return true;
        noteSite(out_.allocSitesByCallee, g, pointee(BO->getLHS()->getType()));
        // redis returns "ptr = extend_to_usable(ptr, n)" through a variable
        // declared from an earlier call, so tracking only the declaration
        // loses the source that carries the alloc_size attribute.
        if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(
                BO->getLHS()->IgnoreParenImpCasts()))
            if (const auto *VD =
                    llvm::dyn_cast<clang::VarDecl>(DRE->getDecl()))
                varSource_[VD->getCanonicalDecl()].insert(g);
        return true;
    }

    // return G(...), and the factory form: T *p = G(...); ... return p;
    bool VisitReturnStmt(clang::ReturnStmt *RS) {
        if (!fn_ || !RS || !RS->getRetValue())
            return true;
        const clang::Expr *rv = RS->getRetValue()->IgnoreParenImpCasts();
        std::set<std::string> srcs;
        if (std::string g = calleeOf(rv); !g.empty())
            srcs.insert(std::move(g));
        else if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(rv))
            if (const auto *VD =
                    llvm::dyn_cast<clang::VarDecl>(DRE->getDecl())) {
                auto it = varSource_.find(VD->getCanonicalDecl());
                if (it != varSource_.end())
                    srcs = it->second;
            }
        // Several sources means several branches reached this return. A
        // function allocates if any path through it does, which is also the
        // direction a hazard detector should err in.
        // A generic allocator is type-agnostic by construction; a constructor
        // that happens to allocate returns its own type, and its allocation is
        // already a direct site in its body. Propagating through both made the
        // closure transitively true and useless: zzlDelete reaches zrealloc in
        // four hops, which put FL020 at 633 findings on the wrong lines.
        const bool generic = fn_->getReturnType()
                                 .getCanonicalType()->isVoidPointerType();
        for (const auto &g : srcs) {
            if (generic)
                out_.returnForwards[threadRoleNodeName(fn_, ctx_)].insert(g);
            noteSite(out_.allocSitesByCallee, g, pointee(fn_->getReturnType()));
        }
        return true;
    }

    // G(x) with x a pointer. If x is one of this function's own parameters,
    // F is a release wrapper exactly when G is.
    bool VisitCallExpr(clang::CallExpr *E) {
        if (!fn_ || !E)
            return true;
        const auto *callee = E->getDirectCallee();
        if (!callee || !callee->getIdentifier())
            return true;
        std::string g = threadRoleNodeName(callee, ctx_);
        classifyDecl(callee, g);
        if (E->getNumArgs() < 1)
            return true;
        const clang::Expr *a0 = E->getArg(0)->IgnoreImpCasts();
        if (clang::QualType pt = pointee(a0->getType()); !pt.isNull())
            noteSite(out_.freeSitesByCallee, g, pt);
        // Any pointer argument, and the parameter may be reached through
        // arithmetic or a local. valkey releases through
        // "prefix = (unsigned char *)ptr - PREFIX_SIZE" and frees the prefix,
        // which is the same release; requiring a bare parameter reference saw
        // neither that nor the local it is assigned to first.
        for (unsigned i = 0; i < E->getNumArgs(); ++i) {
            const clang::Expr *a = E->getArg(i)->IgnoreImpCasts();
            if (!a->getType()->isPointerType() || !mentionsParam(a))
                continue;
            out_.paramForwards[threadRoleNodeName(fn_, ctx_)].insert(g);
            break;
        }
        return true;
    }

private:
    // None of this depends on what the function is called. Clang spells
    // __attribute__((malloc)) as RestrictAttr, there is no MallocAttr, and it
    // synthesizes alloc_size onto libc even where the header omits it.
    void classifyDecl(const clang::FunctionDecl *FD, const std::string &n) {
        if (FD->getBuiltinID() != 0)
            out_.builtinCallees.insert(n);
        // Not the bare malloc attribute: glibc marks fopen and opendir that
        // way too, and they do return a fresh block, but this rule grades
        // caller-sized allocation.
        if (FD->hasAttr<clang::AllocSizeAttr>())
            out_.declaredAllocators.insert(n);
        for (const auto *OA : FD->specific_attrs<clang::OwnershipAttr>()) {
            if (OA->getOwnKind() == clang::OwnershipAttr::Returns)
                out_.declaredAllocators.insert(n);
            else if (OA->getOwnKind() == clang::OwnershipAttr::Takes)
                out_.declaredFreers.insert(n);
        }
    }

    // void* is the release side of the discipline the return path uses: a
    // function taking a typed pointer is doing its own work and merely happens
    // to release. Accepting those ran the closure away, since nearly every
    // function hands some pointer parameter to something that eventually frees.
    bool mentionsParam(const clang::Stmt *S) const {
        if (!S)
            return false;
        if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(S)) {
            const auto *D = DRE->getDecl();
            if (const auto *PV = llvm::dyn_cast<clang::ParmVarDecl>(D))
                return PV->getType().getCanonicalType()->isVoidPointerType();
            if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(D))
                return paramTainted_.count(VD->getCanonicalDecl()) > 0;
        }
        for (const auto *C : S->children())
            if (mentionsParam(C))
                return true;
        return false;
    }

    static clang::QualType pointee(clang::QualType QT) {
        if (QT.isNull())
            return {};
        const auto *PT = QT->getAs<clang::PointerType>();
        return PT ? PT->getPointeeType() : clang::QualType();
    }

    std::string calleeOf(const clang::Expr *E) const {
        const auto *CE = llvm::dyn_cast_or_null<clang::CallExpr>(
            E ? E->IgnoreParenImpCasts() : nullptr);
        if (!CE)
            return {};
        const auto *callee = CE->getDirectCallee();
        if (!callee || !callee->getIdentifier())
            return {};
        return threadRoleNodeName(callee, ctx_);
    }

    std::string typeKey(clang::QualType QT) const {
        if (QT.isNull())
            return {};
        QT = QT.getCanonicalType().getUnqualifiedType();
        if (QT->isVoidType() || QT->isDependentType() || QT->isIncompleteType())
            return {};
        const auto *RD = QT->getAsRecordDecl();
        std::string n = RD ? RD->getCanonicalDecl()->getQualifiedNameAsString()
                           : QT.getAsString();
        return n;
    }

    void noteType(std::map<std::string, std::set<std::string>> &dst,
                  clang::QualType QT) {
        std::string n = typeKey(QT);
        if (!n.empty())
            dst[n].insert(threadRoleNodeName(fn_, ctx_));
    }

    void noteSite(std::map<std::string, std::set<std::string>> &dst,
                  const std::string &callee, clang::QualType QT) {
        std::string n = typeKey(QT);
        if (!n.empty())
            dst[callee].insert(threadRoleNodeName(fn_, ctx_) + "|" + n);
    }

    clang::ASTContext &ctx_;
    ThreadRoleSummary &out_;
    const clang::FunctionDecl *fn_ = nullptr;
    std::map<const clang::VarDecl *, std::set<std::string>> varSource_;
    std::set<const clang::VarDecl *> paramTainted_;
};

class VocabularyConsumer : public clang::ASTConsumer {
public:
    explicit VocabularyConsumer(ThreadRoleSummary &out) : out_(out) {}

    void HandleTranslationUnit(clang::ASTContext &Ctx) override {
        if (Ctx.getDiagnostics().hasFatalErrorOccurred())
            return;
        collectAllocOwnership(Ctx, out_);
    }

private:
    ThreadRoleSummary &out_;
};

} // anonymous namespace

void collectAllocOwnership(clang::ASTContext &Ctx, ThreadRoleSummary &out) {
    AllocOwnershipVisitor v(Ctx, out);
    v.TraverseDecl(Ctx.getTranslationUnitDecl());
}

void collectReadFiles(clang::CompilerInstance &CI,
                      std::vector<std::string> &out) {
    const auto &SM = CI.getSourceManager();
    std::set<std::string> uniq;
    for (auto it = SM.fileinfo_begin(); it != SM.fileinfo_end(); ++it) {
        llvm::StringRef n = it->first.getName();
        if (!n.empty())
            uniq.insert(n.str());
    }
    // The main file is not always among the fileinfos, and a cache entry that
    // does not depend on its own source is the worst possible entry.
    if (auto id = SM.getMainFileID(); id.isValid())
        if (const auto *fe = SM.getFileEntryForID(id))
            uniq.insert(fe->getName().str());
    out.assign(uniq.begin(), uniq.end());
}

std::unique_ptr<clang::ASTConsumer>
VocabularyAction::CreateASTConsumer(clang::CompilerInstance & /*CI*/,
                                    llvm::StringRef /*file*/) {
    return std::make_unique<VocabularyConsumer>(out_);
}

void VocabularyAction::EndSourceFileAction() {
    if (deps_)
        collectReadFiles(getCompilerInstance(), *deps_);
}

} // namespace lshaz
