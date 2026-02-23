#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

namespace {

struct StdFuncSite {
    clang::SourceLocation loc;
    enum Kind { Invoke, Construct, Parameter } kind;
    unsigned inLoop = 0;
};

class StdFuncVisitor : public clang::RecursiveASTVisitor<StdFuncVisitor> {
public:
    bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *E) {
        // operator() on std::function
        if (E->getOperator() != clang::OO_Call)
            return true;

        if (E->getNumArgs() < 1)
            return true;

        clang::QualType QT = E->getArg(0)->getType().getCanonicalType();
        if (isStdFunction(QT))
            sites_.push_back({E->getBeginLoc(), StdFuncSite::Invoke, inLoop_});

        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr *E) {
        if (const auto *CD = E->getConstructor()) {
            std::string parent = CD->getParent()->getQualifiedNameAsString();
            if (parent.find("std::function") != std::string::npos) {
                sites_.push_back({E->getBeginLoc(), StdFuncSite::Construct, inLoop_});
            }
        }
        return true;
    }

    bool VisitCallExpr(clang::CallExpr *E) {
        // Direct call through a std::function variable.
        if (const auto *callee = llvm::dyn_cast_or_null<clang::DeclRefExpr>(
                E->getCallee()->IgnoreImplicit())) {
            clang::QualType QT = callee->getType().getCanonicalType();
            if (isStdFunction(QT))
                sites_.push_back({E->getBeginLoc(), StdFuncSite::Invoke, inLoop_});
        }
        return true;
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<StdFuncVisitor>::TraverseForStmt(S);
        --inLoop_;
        return r;
    }

    bool TraverseWhileStmt(clang::WhileStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<StdFuncVisitor>::TraverseWhileStmt(S);
        --inLoop_;
        return r;
    }

    bool TraverseDoStmt(clang::DoStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<StdFuncVisitor>::TraverseDoStmt(S);
        --inLoop_;
        return r;
    }

    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<StdFuncVisitor>::TraverseCXXForRangeStmt(S);
        --inLoop_;
        return r;
    }

    const std::vector<StdFuncSite> &sites() const { return sites_; }

private:
    bool isStdFunction(clang::QualType QT) const {
        return QT.getAsString().find("std::function") != std::string::npos;
    }

    std::vector<StdFuncSite> sites_;
    unsigned inLoop_ = 0;
};

} // anonymous namespace

class FL031_StdFunctionHotPath : public Rule {
public:
    std::string_view getID() const override { return "FL031"; }
    std::string_view getTitle() const override { return "std::function in Hot Path"; }
    Severity getBaseSeverity() const override { return Severity::High; }

    std::string_view getHardwareMechanism() const override {
        return "std::function uses type-erased callable storage. "
               "Invocation requires indirect call (BTB pressure). "
               "Construction may heap-allocate if callable exceeds SBO "
               "(typically 16-32B). Prevents inlining.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle &Oracle,
                 std::vector<Diagnostic> &out) override {

        const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return;

        if (!Oracle.isFunctionHot(FD))
            return;

        // Also flag std::function parameters — invocation in hot function body
        // is the concern regardless of where it was constructed.
        bool hasStdFuncParam = false;
        for (const auto *param : FD->parameters()) {
            clang::QualType QT = param->getType().getCanonicalType();
            if (QT.getAsString().find("std::function") != std::string::npos) {
                hasStdFuncParam = true;
                break;
            }
        }

        StdFuncVisitor visitor;
        visitor.TraverseStmt(FD->getBody());

        if (visitor.sites().empty() && !hasStdFuncParam)
            return;

        const auto &SM = Ctx.getSourceManager();

        for (const auto &site : visitor.sites()) {
            Severity sev = Severity::High;
            std::vector<std::string> escalations;

            const char *kindStr = "invocation";
            if (site.kind == StdFuncSite::Construct)
                kindStr = "construction";

            if (site.inLoop) {
                sev = Severity::Critical;
                escalations.push_back(
                    "std::function used inside loop: repeated indirect call "
                    "and potential per-iteration heap allocation");
            }

            if (site.kind == StdFuncSite::Construct) {
                escalations.push_back(
                    "std::function constructed in hot path: may heap-allocate "
                    "if callable exceeds SBO threshold (~16-32B)");
            }

            Diagnostic diag;
            diag.ruleID    = "FL031";
            diag.title     = "std::function in Hot Path";
            diag.severity  = sev;
            diag.confidence = 0.80;
            diag.evidenceTier = EvidenceTier::Proven;

            if (site.loc.isValid()) {
                diag.location.file   = SM.getFilename(SM.getSpellingLoc(site.loc)).str();
                diag.location.line   = SM.getSpellingLineNumber(site.loc);
                diag.location.column = SM.getSpellingColumnNumber(site.loc);
            }

            std::ostringstream hw;
            hw << "std::function " << kindStr << " in hot function '"
               << FD->getQualifiedNameAsString()
               << "'. Type erasure forces indirect call through function pointer "
               << "(BTB lookup, pipeline flush on mispredict). "
               << "Prevents compiler inlining of the callable.";
            diag.hardwareReasoning = hw.str();

            std::ostringstream ev;
            ev << "std_function_" << kindStr
               << "; caller=" << FD->getQualifiedNameAsString()
               << "; in_loop=" << (site.inLoop ? "yes" : "no")
               << "; hot_path=true";
            diag.structuralEvidence = ev.str();

            diag.mitigation =
                "Use template parameter for callable type. "
                "Use auto lambda. "
                "Use raw function pointer if target is known. "
                "Use std::variant + visitor for closed type sets.";

            diag.escalations = std::move(escalations);
            out.push_back(std::move(diag));
        }
    }
};

FAULTLINE_REGISTER_RULE(FL031_StdFunctionHotPath)

} // namespace faultline
