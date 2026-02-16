#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

namespace {

struct SeqCstSite {
    clang::SourceLocation loc;
    std::string atomicOp;   // "load", "store", "fetch_add", "exchange", etc.
    std::string varName;
    unsigned inLoop = 0;
};

// Detect memory_order_seq_cst usage on std::atomic member calls.
// seq_cst is the default when no order is specified, so we flag both
// explicit seq_cst and implicit (no argument) cases.
class SeqCstVisitor : public clang::RecursiveASTVisitor<SeqCstVisitor> {
public:
    explicit SeqCstVisitor(clang::ASTContext &Ctx) : ctx_(Ctx) {}

    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *E) {
        const auto *MD = E->getMethodDecl();
        if (!MD)
            return true;

        std::string methodName = MD->getNameAsString();

        // Filter to atomic operations.
        static const char *atomicOps[] = {
            "load", "store", "exchange",
            "compare_exchange_weak", "compare_exchange_strong",
            "fetch_add", "fetch_sub", "fetch_and", "fetch_or", "fetch_xor",
            "notify_one", "notify_all", "wait"
        };

        bool isAtomicOp = false;
        for (const auto *op : atomicOps) {
            if (methodName == op) {
                isAtomicOp = true;
                break;
            }
        }
        if (!isAtomicOp)
            return true;

        // Check if the object is std::atomic.
        const auto *obj = E->getImplicitObjectArgument();
        if (!obj)
            return true;

        std::string typeName = obj->getType().getCanonicalType().getAsString();
        if (typeName.find("atomic") == std::string::npos)
            return true;

        // Determine if seq_cst is used (explicit or implicit default).
        bool isSeqCst = true; // Default assumption: no order arg = seq_cst.

        // For load/store/exchange/fetch_*, the memory_order is typically
        // the last argument. If it's explicitly relaxed/acquire/release/acq_rel,
        // we don't flag.
        for (unsigned i = 0; i < E->getNumArgs(); ++i) {
            const auto *arg = E->getArg(i)->IgnoreImplicit();
            if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(arg)) {
                std::string argName = DRE->getDecl()->getNameAsString();
                if (argName.find("relaxed") != std::string::npos ||
                    argName.find("acquire") != std::string::npos ||
                    argName.find("release") != std::string::npos ||
                    argName.find("acq_rel") != std::string::npos ||
                    argName.find("consume") != std::string::npos) {
                    isSeqCst = false;
                }
            }
        }

        if (!isSeqCst)
            return true;

        // Extract variable name from the object expression.
        std::string varName = "<unknown>";
        if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(
                obj->IgnoreImplicit())) {
            varName = ME->getMemberDecl()->getNameAsString();
        } else if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(
                       obj->IgnoreImplicit())) {
            varName = DRE->getDecl()->getNameAsString();
        }

        sites_.push_back({E->getBeginLoc(), methodName, varName, inLoop_});
        return true;
    }

    // Also catch operator overloads on atomics (++, --, +=, etc.)
    // These default to seq_cst.
    bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *E) {
        if (E->getNumArgs() < 1)
            return true;

        std::string typeName =
            E->getArg(0)->getType().getCanonicalType().getAsString();
        if (typeName.find("atomic") == std::string::npos)
            return true;

        auto op = E->getOperator();
        std::string opName;
        switch (op) {
            case clang::OO_PlusPlus:  opName = "operator++"; break;
            case clang::OO_MinusMinus: opName = "operator--"; break;
            case clang::OO_PlusEqual: opName = "operator+="; break;
            case clang::OO_MinusEqual: opName = "operator-="; break;
            case clang::OO_AmpEqual:  opName = "operator&="; break;
            case clang::OO_PipeEqual: opName = "operator|="; break;
            case clang::OO_CaretEqual: opName = "operator^="; break;
            default: return true;
        }

        // These operators always use seq_cst — no way to specify order.
        sites_.push_back({E->getBeginLoc(), opName, "<atomic>", inLoop_});
        return true;
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<SeqCstVisitor>::TraverseForStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseWhileStmt(clang::WhileStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<SeqCstVisitor>::TraverseWhileStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseDoStmt(clang::DoStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<SeqCstVisitor>::TraverseDoStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<SeqCstVisitor>::TraverseCXXForRangeStmt(S);
        --inLoop_;
        return r;
    }

    const std::vector<SeqCstSite> &sites() const { return sites_; }

private:
    clang::ASTContext &ctx_;
    std::vector<SeqCstSite> sites_;
    unsigned inLoop_ = 0;
};

} // anonymous namespace

class FL010_OverlyStrongOrdering : public Rule {
public:
    std::string_view getID() const override { return "FL010"; }
    std::string_view getTitle() const override { return "Overly Strong Atomic Ordering"; }
    Severity getBaseSeverity() const override { return Severity::High; }

    std::string_view getHardwareMechanism() const override {
        return "memory_order_seq_cst emits full memory fence (MFENCE or "
               "LOCK-prefixed instruction on x86-64). Forces store buffer "
               "drain and pipeline serialization. On TSO, acquire/release "
               "semantics are free for loads/stores — seq_cst pays unnecessary "
               "fence cost.";
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

        SeqCstVisitor visitor(Ctx);
        visitor.TraverseStmt(FD->getBody());

        if (visitor.sites().empty())
            return;

        const auto &SM = Ctx.getSourceManager();
        unsigned atomicCount = visitor.sites().size();

        for (const auto &site : visitor.sites()) {
            Severity sev = Severity::High;
            std::vector<std::string> escalations;

            if (site.inLoop) {
                sev = Severity::Critical;
                escalations.push_back(
                    "seq_cst atomic inside loop: repeated store buffer drains, "
                    "sustained pipeline serialization");
            }

            if (atomicCount > 1) {
                escalations.push_back(
                    "Multiple seq_cst atomics in same function: compounding "
                    "fence overhead per invocation");
            }

            Diagnostic diag;
            diag.ruleID    = "FL010";
            diag.title     = "Overly Strong Atomic Ordering";
            diag.severity  = sev;
            diag.confidence = 0.80;

            if (site.loc.isValid()) {
                diag.location.file   = SM.getFilename(SM.getSpellingLoc(site.loc)).str();
                diag.location.line   = SM.getSpellingLineNumber(site.loc);
                diag.location.column = SM.getSpellingColumnNumber(site.loc);
            }

            std::ostringstream hw;
            hw << "atomic " << site.atomicOp << " on '" << site.varName
               << "' uses seq_cst ordering in hot function '"
               << FD->getQualifiedNameAsString()
               << "'. On x86-64 TSO, this emits MFENCE/LOCK prefix forcing "
               << "store buffer drain (~20-50 cycle penalty). "
               << "acquire/release is free for loads/stores on TSO.";
            diag.hardwareReasoning = hw.str();

            std::ostringstream ev;
            ev << "op=" << site.atomicOp
               << "; var=" << site.varName
               << "; ordering=seq_cst"
               << "; function=" << FD->getQualifiedNameAsString()
               << "; in_loop=" << (site.inLoop ? "yes" : "no")
               << "; total_atomics_in_func=" << atomicCount;
            diag.structuralEvidence = ev.str();

            diag.mitigation =
                "Use memory_order_acquire for loads, memory_order_release for "
                "stores. Use memory_order_acq_rel for RMW operations. "
                "Use memory_order_relaxed where ordering is not required. "
                "On x86-64 TSO, acquire/release have zero fence cost.";

            diag.escalations = std::move(escalations);
            out.push_back(std::move(diag));
        }
    }
};

FAULTLINE_REGISTER_RULE(FL010_OverlyStrongOrdering)

} // namespace faultline
