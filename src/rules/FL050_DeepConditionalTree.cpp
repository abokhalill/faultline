#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>

#include <algorithm>
#include <sstream>

namespace faultline {

namespace {

struct BranchSite {
    clang::SourceLocation loc;
    unsigned depth;
    bool isSwitchStmt;
    unsigned switchCases; // only for switch
};

class BranchDepthVisitor : public clang::RecursiveASTVisitor<BranchDepthVisitor> {
public:
    explicit BranchDepthVisitor(unsigned threshold) : threshold_(threshold) {}

    bool TraverseIfStmt(clang::IfStmt *S) {
        ++depth_;
        maxDepth_ = std::max(maxDepth_, depth_);

        if (depth_ >= threshold_) {
            sites_.push_back({S->getIfLoc(), depth_, false, 0});
        }

        bool r = clang::RecursiveASTVisitor<BranchDepthVisitor>::TraverseIfStmt(S);
        --depth_;
        return r;
    }

    bool VisitSwitchStmt(clang::SwitchStmt *S) {
        unsigned caseCount = 0;
        for (auto *sc = S->getSwitchCaseList(); sc; sc = sc->getNextSwitchCase())
            ++caseCount;

        if (caseCount >= 8) {
            sites_.push_back({S->getSwitchLoc(), depth_, true, caseCount});
        }
        return true;
    }

    const std::vector<BranchSite> &sites() const { return sites_; }
    unsigned maxDepth() const { return maxDepth_; }

private:
    unsigned threshold_;
    unsigned depth_ = 0;
    unsigned maxDepth_ = 0;
    std::vector<BranchSite> sites_;
};

} // anonymous namespace

class FL050_DeepConditionalTree : public Rule {
public:
    std::string_view getID() const override { return "FL050"; }
    std::string_view getTitle() const override { return "Deep Conditional Tree in Hot Path"; }
    Severity getBaseSeverity() const override { return Severity::Medium; }

    std::string_view getHardwareMechanism() const override {
        return "Deeply nested conditionals increase branch misprediction "
               "surface. Each unpredictable branch costs ~14-20 cycles "
               "(pipeline flush). Large switch statements on non-constexpr "
               "values pressure the BTB and I-cache.";
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

        const unsigned threshold = 4; // Per RULEBOOK: configurable

        BranchDepthVisitor visitor(threshold);
        visitor.TraverseStmt(FD->getBody());

        if (visitor.sites().empty())
            return;

        const auto &SM = Ctx.getSourceManager();

        // Deduplicate: only emit for the deepest nesting point and large switches.
        bool emittedNested = false;

        for (const auto &site : visitor.sites()) {
            if (!site.isSwitchStmt && emittedNested)
                continue;

            Severity sev = Severity::Medium;
            std::vector<std::string> escalations;

            if (site.isSwitchStmt) {
                sev = Severity::High;
                escalations.push_back(
                    "Large switch (" + std::to_string(site.switchCases) +
                    " cases): BTB capacity pressure, I-cache bloat from "
                    "jump table expansion");
            } else {
                if (site.depth >= 6) {
                    sev = Severity::High;
                    escalations.push_back(
                        "Nesting depth " + std::to_string(site.depth) +
                        ": high branch entropy, compounding misprediction cost");
                }
                emittedNested = true;
            }

            Diagnostic diag;
            diag.ruleID    = "FL050";
            diag.title     = "Deep Conditional Tree in Hot Path";
            diag.severity  = sev;
            diag.confidence = 0.50; // Per RULEBOOK: Low-Medium

            if (site.loc.isValid()) {
                diag.location.file   = SM.getFilename(SM.getSpellingLoc(site.loc)).str();
                diag.location.line   = SM.getSpellingLineNumber(site.loc);
                diag.location.column = SM.getSpellingColumnNumber(site.loc);
            }

            std::ostringstream hw;
            if (site.isSwitchStmt) {
                hw << "switch statement with " << site.switchCases
                   << " cases in hot function '"
                   << FD->getQualifiedNameAsString()
                   << "'. Non-constexpr switch generates indirect jump table. "
                   << "BTB must predict target from " << site.switchCases
                   << " possibilities. I-cache footprint scales with case count.";
            } else {
                hw << "Conditional nesting depth " << site.depth
                   << " in hot function '" << FD->getQualifiedNameAsString()
                   << "'. Each nested branch is a prediction point. "
                   << "Deep trees create correlated misprediction chains "
                   << "that defeat pattern-based predictors.";
            }
            diag.hardwareReasoning = hw.str();

            std::ostringstream ev;
            ev << "function=" << FD->getQualifiedNameAsString()
               << "; type=" << (site.isSwitchStmt ? "switch" : "nested_if")
               << "; depth=" << site.depth
               << "; max_depth=" << visitor.maxDepth();
            if (site.isSwitchStmt)
                ev << "; cases=" << site.switchCases;
            diag.structuralEvidence = ev.str();

            diag.mitigation =
                "Use table-driven dispatch. "
                "Flatten conditional logic with early returns. "
                "Precompute decision trees. "
                "Use __builtin_expect for predictable branches.";

            diag.escalations = std::move(escalations);
            out.push_back(std::move(diag));
        }
    }
};

FAULTLINE_REGISTER_RULE(FL050_DeepConditionalTree)

} // namespace faultline
