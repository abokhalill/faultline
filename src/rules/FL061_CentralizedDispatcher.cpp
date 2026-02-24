#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

namespace {

struct DispatchInfo {
    unsigned virtualCalls = 0;
    unsigned indirectCalls = 0;   // std::function operator()
    unsigned switchCases = 0;
    unsigned callees = 0;         // distinct function calls
    bool hasLoop = false;
};

class DispatchVisitor : public clang::RecursiveASTVisitor<DispatchVisitor> {
public:
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *E) {
        if (const auto *MD = E->getMethodDecl()) {
            if (MD->isVirtual())
                ++info_.virtualCalls;
        }
        return true;
    }

    bool VisitCallExpr(clang::CallExpr *E) {
        ++info_.callees;
        return true;
    }

    bool VisitSwitchStmt(clang::SwitchStmt *S) {
        unsigned cases = 0;
        for (auto *sc = S->getSwitchCaseList(); sc; sc = sc->getNextSwitchCase())
            ++cases;
        info_.switchCases = std::max(info_.switchCases, cases);
        return true;
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        info_.hasLoop = true;
        return clang::RecursiveASTVisitor<DispatchVisitor>::TraverseForStmt(S);
    }
    bool TraverseWhileStmt(clang::WhileStmt *S) {
        info_.hasLoop = true;
        return clang::RecursiveASTVisitor<DispatchVisitor>::TraverseWhileStmt(S);
    }
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        info_.hasLoop = true;
        return clang::RecursiveASTVisitor<DispatchVisitor>::TraverseCXXForRangeStmt(S);
    }

    const DispatchInfo &info() const { return info_; }

private:
    DispatchInfo info_;
};

} // anonymous namespace

class FL061_CentralizedDispatcher : public Rule {
public:
    std::string_view getID() const override { return "FL061"; }
    std::string_view getTitle() const override { return "Centralized Dispatcher Bottleneck"; }
    Severity getBaseSeverity() const override { return Severity::High; }

    std::string_view getHardwareMechanism() const override {
        return "Single-point fan-out dispatcher serializes all message processing "
               "through one function. Under load, this creates: instruction cache "
               "pressure from large dispatch body, branch misprediction from "
               "polymorphic dispatch, and prevents per-core locality of message "
               "handling state.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle &Oracle,
                 const Config & /*Cfg*/,
                 std::vector<Diagnostic> &out) override {

        const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return;

        if (!Oracle.isFunctionHot(FD))
            return;

        DispatchVisitor visitor;
        visitor.TraverseStmt(FD->getBody());

        const auto &info = visitor.info();

        // Heuristic: centralized dispatcher has high fan-out.
        // Threshold: 5+ callees OR large switch OR 3+ virtual calls.
        bool isDispatcher = false;
        std::string reason;

        if (info.callees >= 8) {
            isDispatcher = true;
            reason = std::to_string(info.callees) + " call sites (high fan-out)";
        } else if (info.switchCases >= 6 && info.callees >= 3) {
            isDispatcher = true;
            reason = std::to_string(info.switchCases) + "-case switch with " +
                     std::to_string(info.callees) + " call sites";
        } else if (info.virtualCalls >= 3) {
            isDispatcher = true;
            reason = std::to_string(info.virtualCalls) +
                     " virtual dispatch sites (polymorphic fan-out)";
        }

        if (!isDispatcher)
            return;

        Severity sev = Severity::High;
        std::vector<std::string> escalations;

        if (info.hasLoop) {
            sev = Severity::Critical;
            escalations.push_back(
                "Dispatch loop: per-iteration fan-out amplifies I-cache "
                "and BTB pressure");
        }

        if (info.virtualCalls >= 3 && info.switchCases >= 4) {
            sev = Severity::Critical;
            escalations.push_back(
                "Mixed dispatch: switch + virtual calls compound "
                "branch misprediction surface");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = FD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL061";
        diag.title     = "Centralized Dispatcher Bottleneck";
        diag.severity  = sev;
        diag.confidence = 0.55;
        diag.evidenceTier = EvidenceTier::Speculative;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Hot function '" << FD->getQualifiedNameAsString()
           << "' exhibits centralized dispatcher pattern: " << reason
           << ". Single-point fan-out serializes all processing, "
           << "pressures I-cache with large dispatch body, and "
           << "creates BTB contention from multiple indirect targets.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "function=" << FD->getQualifiedNameAsString()
           << "; callees=" << info.callees
           << "; virtual_calls=" << info.virtualCalls
           << "; switch_cases=" << info.switchCases
           << "; has_loop=" << (info.hasLoop ? "yes" : "no");
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Partition dispatch by message type to separate handlers. "
            "Use compile-time dispatch (templates, CRTP) where type set is closed. "
            "Shard by core to eliminate cross-core contention on dispatcher state. "
            "Consider table-driven dispatch with function pointer arrays.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL061_CentralizedDispatcher)

} // namespace faultline
