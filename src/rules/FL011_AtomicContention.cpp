#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/analysis/EscapeAnalysis.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

namespace {

struct AtomicWriteSite {
    clang::SourceLocation loc;
    std::string op;
    std::string varName;
    unsigned inLoop = 0;
};

class AtomicWriteVisitor : public clang::RecursiveASTVisitor<AtomicWriteVisitor> {
public:
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *E) {
        const auto *MD = E->getMethodDecl();
        if (!MD)
            return true;

        std::string name = MD->getNameAsString();

        // Write operations on atomics.
        static const char *writeOps[] = {
            "store", "exchange",
            "compare_exchange_weak", "compare_exchange_strong",
            "fetch_add", "fetch_sub", "fetch_and", "fetch_or", "fetch_xor"
        };

        bool isWrite = false;
        for (const auto *op : writeOps) {
            if (name == op) { isWrite = true; break; }
        }
        if (!isWrite)
            return true;

        const auto *obj = E->getImplicitObjectArgument();
        if (!obj)
            return true;

        std::string typeName = obj->getType().getCanonicalType().getAsString();
        if (typeName.find("atomic") == std::string::npos)
            return true;

        std::string varName = "<unknown>";
        if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(
                obj->IgnoreImplicit()))
            varName = ME->getMemberDecl()->getNameAsString();
        else if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(
                     obj->IgnoreImplicit()))
            varName = DRE->getDecl()->getNameAsString();

        sites_.push_back({E->getBeginLoc(), name, varName, inLoop_});
        return true;
    }

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
            case clang::OO_PlusPlus:   opName = "operator++"; break;
            case clang::OO_MinusMinus: opName = "operator--"; break;
            case clang::OO_PlusEqual:  opName = "operator+="; break;
            case clang::OO_MinusEqual: opName = "operator-="; break;
            case clang::OO_AmpEqual:   opName = "operator&="; break;
            case clang::OO_PipeEqual:  opName = "operator|="; break;
            case clang::OO_CaretEqual: opName = "operator^="; break;
            case clang::OO_Equal:      opName = "operator="; break;
            default: return true;
        }

        sites_.push_back({E->getBeginLoc(), opName, "<atomic>", inLoop_});
        return true;
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AtomicWriteVisitor>::TraverseForStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseWhileStmt(clang::WhileStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AtomicWriteVisitor>::TraverseWhileStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseDoStmt(clang::DoStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AtomicWriteVisitor>::TraverseDoStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AtomicWriteVisitor>::TraverseCXXForRangeStmt(S);
        --inLoop_;
        return r;
    }

    const std::vector<AtomicWriteSite> &sites() const { return sites_; }

private:
    std::vector<AtomicWriteSite> sites_;
    unsigned inLoop_ = 0;
};

} // anonymous namespace

class FL011_AtomicContention : public Rule {
public:
    std::string_view getID() const override { return "FL011"; }
    std::string_view getTitle() const override { return "Atomic Contention Hotspot"; }
    Severity getBaseSeverity() const override { return Severity::Critical; }

    std::string_view getHardwareMechanism() const override {
        return "Cache line ownership thrashing via MESI RFO (Read-For-Ownership). "
               "Each atomic write from a different core forces exclusive ownership "
               "transfer (~40-100ns cross-core, ~100-300ns cross-socket). "
               "Store buffer pressure from sustained atomic writes.";
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

        AtomicWriteVisitor visitor;
        visitor.TraverseStmt(FD->getBody());

        if (visitor.sites().empty())
            return;

        const auto &SM = Ctx.getSourceManager();
        unsigned writeCount = visitor.sites().size();

        // Only flag if there are multiple atomic writes (contention signal)
        // or writes inside loops (sustained pressure).
        bool hasLoopWrite = false;
        for (const auto &s : visitor.sites()) {
            if (s.inLoop) { hasLoopWrite = true; break; }
        }

        if (writeCount < 2 && !hasLoopWrite)
            return;

        // Emit one diagnostic per function summarizing the contention risk.
        Severity sev = Severity::Critical;
        std::vector<std::string> escalations;

        if (writeCount >= 3) {
            escalations.push_back(
                "3+ atomic writes per invocation: high store buffer pressure, "
                "sustained RFO traffic");
        }

        if (hasLoopWrite) {
            escalations.push_back(
                "Atomic write inside loop: per-iteration cache line ownership "
                "transfer, store buffer saturation risk");
        }

        auto loc = FD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL011";
        diag.title     = "Atomic Contention Hotspot";
        diag.severity  = sev;
        diag.confidence = hasLoopWrite ? 0.80 : 0.65;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Hot function '" << FD->getQualifiedNameAsString()
           << "' contains " << writeCount << " atomic write(s). "
           << "Under multi-core contention, each write triggers RFO "
           << "cache line transfer. Multiple writes compound store buffer "
           << "drain latency and coherence traffic.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "function=" << FD->getQualifiedNameAsString()
           << "; atomic_writes=" << writeCount
           << "; loop_writes=" << (hasLoopWrite ? "yes" : "no")
           << "; ops=[";
        for (size_t i = 0; i < visitor.sites().size(); ++i) {
            ev << visitor.sites()[i].op << "(" << visitor.sites()[i].varName << ")";
            if (i + 1 < visitor.sites().size()) ev << ", ";
        }
        ev << "]";
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Shard atomic state per-core to eliminate cross-core RFO. "
            "Batch updates to reduce write frequency. "
            "Redesign ownership model to single-writer pattern. "
            "Consider thread-local accumulation with periodic merge.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL011_AtomicContention)

} // namespace faultline
