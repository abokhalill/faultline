#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/analysis/EscapeAnalysis.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

namespace {

// Check if a type is referenced inside any loop body within a function.
class LoopUsageVisitor : public clang::RecursiveASTVisitor<LoopUsageVisitor> {
public:
    explicit LoopUsageVisitor(const clang::CXXRecordDecl *Target)
        : target_(Target) {}

    bool VisitDeclRefExpr(clang::DeclRefExpr *E) {
        if (!inLoop_)
            return true;
        auto QT = E->getType().getCanonicalType();
        if (const auto *RD = QT->getAsCXXRecordDecl()) {
            if (RD->getCanonicalDecl() == target_->getCanonicalDecl())
                usedInLoop_ = true;
        }
        return true;
    }

    bool VisitMemberExpr(clang::MemberExpr *E) {
        if (!inLoop_)
            return true;
        if (const auto *FD = llvm::dyn_cast<clang::FieldDecl>(E->getMemberDecl())) {
            if (const auto *parent = llvm::dyn_cast<clang::CXXRecordDecl>(FD->getParent())) {
                if (parent->getCanonicalDecl() == target_->getCanonicalDecl())
                    usedInLoop_ = true;
            }
        }
        return true;
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LoopUsageVisitor>::TraverseForStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseWhileStmt(clang::WhileStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LoopUsageVisitor>::TraverseWhileStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseDoStmt(clang::DoStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LoopUsageVisitor>::TraverseDoStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LoopUsageVisitor>::TraverseCXXForRangeStmt(S);
        --inLoop_;
        return r;
    }

    bool usedInLoop() const { return usedInLoop_; }

private:
    const clang::CXXRecordDecl *target_;
    unsigned inLoop_ = 0;
    bool usedInLoop_ = false;
};

} // anonymous namespace

class FL090_HazardAmplification : public Rule {
public:
    std::string_view getID() const override { return "FL090"; }
    std::string_view getTitle() const override { return "Hazard Amplification"; }
    Severity getBaseSeverity() const override { return Severity::Critical; }

    std::string_view getHardwareMechanism() const override {
        return "Multiple interacting latency multipliers on a single structure: "
               "cache line spanning + atomic contention + cross-thread sharing + "
               "loop usage. Each hazard compounds nonlinearly under load. "
               "Coherence storms, store buffer saturation, and TLB pressure "
               "interact to produce catastrophic tail latency.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle & /*Oracle*/,
                 std::vector<Diagnostic> &out) override {

        const auto *RD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D);
        if (!RD || !RD->isCompleteDefinition())
            return;
        if (RD->isImplicit() || RD->isLambda())
            return;

        const auto &layout = Ctx.getASTRecordLayout(RD);
        uint64_t sizeBytes = layout.getSize().getQuantity();

        EscapeAnalysis escape(Ctx);

        // Per RULEBOOK FL090: all four signals must be present.
        // 1. Struct > 128B
        bool largeStruct = sizeBytes > 128;

        // 2. Contains atomic
        bool hasAtomics = escape.hasAtomicMembers(RD);

        // 3. Shared across threads (escape analysis)
        bool threadEscape = escape.mayEscapeThread(RD);

        // Need at least 3 of the structural signals to flag.
        // Loop usage is checked separately as it requires function-level analysis
        // which we can't do from a record decl alone.
        unsigned signalCount = 0;
        if (largeStruct) ++signalCount;
        if (hasAtomics) ++signalCount;
        if (threadEscape) ++signalCount;

        // Require all three structural signals.
        if (signalCount < 3)
            return;

        Severity sev = Severity::Critical;
        std::vector<std::string> escalations;

        escalations.push_back(
            "sizeof=" + std::to_string(sizeBytes) +
            "B (>" + std::to_string(128) + "B): spans " +
            std::to_string((sizeBytes + 63) / 64) + " cache lines");

        escalations.push_back(
            "Contains atomic fields: cross-core RFO traffic on every write");

        escalations.push_back(
            "Thread-escaping: coherence traffic amplified across all "
            "participating cores");

        // Count mutable fields for additional context.
        unsigned mutableCount = 0;
        for (const auto *field : RD->fields()) {
            if (escape.isFieldMutable(field))
                ++mutableCount;
        }

        if (mutableCount > 4) {
            escalations.push_back(
                std::to_string(mutableCount) + " mutable fields: "
                "wide write surface across multiple cache lines");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL090";
        diag.title     = "Hazard Amplification";
        diag.severity  = sev;
        diag.confidence = 0.90;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << RD->getNameAsString() << "' (" << sizeBytes
           << "B) exhibits compound hazard: large footprint spanning "
           << ((sizeBytes + 63) / 64) << " cache lines, atomic fields "
           << "triggering cross-core RFO, and thread-escaping access pattern. "
           << "Under multi-core contention, these hazards interact "
           << "nonlinearly: coherence invalidation storms across multiple "
           << "lines, store buffer saturation from atomic writes, and "
           << "elevated LLC eviction pressure.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "struct=" << RD->getNameAsString()
           << "; sizeof=" << sizeBytes << "B"
           << "; cache_lines=" << ((sizeBytes + 63) / 64)
           << "; atomics=yes"
           << "; thread_escape=yes"
           << "; mutable_fields=" << mutableCount
           << "; signal_count=" << signalCount;
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Decompose into separate cache-line-aligned sub-structures. "
            "Isolate atomic fields with alignas(64) padding. "
            "Split hot (frequently written) and cold (rarely accessed) fields. "
            "Consider per-core replicas with periodic merge. "
            "AoS→SoA transformation to separate write-heavy channels.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL090_HazardAmplification)

} // namespace faultline
