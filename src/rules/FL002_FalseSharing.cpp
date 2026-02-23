#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/analysis/CacheLineMap.h"
#include "faultline/analysis/EscapeAnalysis.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

class FL002_FalseSharing : public Rule {
public:
    std::string_view getID() const override { return "FL002"; }
    std::string_view getTitle() const override { return "False Sharing Candidate"; }
    Severity getBaseSeverity() const override { return Severity::Critical; }

    std::string_view getHardwareMechanism() const override {
        return "MESI invalidation ping-pong across cores due to shared "
               "cache line writes. Each write by one core forces invalidation "
               "of the line in all other cores' L1/L2, triggering RFO traffic.";
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

        EscapeAnalysis escape(Ctx);
        if (!escape.mayEscapeThread(RD))
            return;

        CacheLineMap map(RD, Ctx);

        auto atomicPairs = map.atomicPairsOnSameLine();
        auto mutablePairs = map.mutablePairsOnSameLine();
        if (mutablePairs.empty())
            return;

        bool hasAtomicPairs = !atomicPairs.empty();
        auto fsCandidateLines = map.falseSharingCandidateLines();

        // Without atomic pairs on the same line, we cannot statically prove
        // that different threads write different fields. Require at least
        // atomic fields in the struct for non-atomic-pair cases.
        if (!hasAtomicPairs && map.totalAtomicFields() == 0)
            return;

        Severity sev = hasAtomicPairs ? Severity::Critical : Severity::High;
        std::vector<std::string> escalations;

        for (const auto &pair : atomicPairs) {
            escalations.push_back(
                "atomic fields '" + pair.a->name + "' and '" + pair.b->name +
                "' share line " + std::to_string(pair.lineIndex) +
                ": guaranteed cross-core invalidation on write");
        }

        for (auto lineIdx : fsCandidateLines) {
            const auto &bucket = map.buckets()[lineIdx];
            escalations.push_back(
                "line " + std::to_string(lineIdx) + ": " +
                std::to_string(bucket.atomicCount) + " atomic + " +
                std::to_string(bucket.mutableCount - bucket.atomicCount) +
                " non-atomic mutable field(s) — mixed write surface");
        }

        double confidence = 0.55;
        if (hasAtomicPairs)
            confidence = 0.88;
        else if (map.totalAtomicFields() > 0)
            confidence = 0.68;

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL002";
        diag.title     = "False Sharing Candidate";
        diag.severity  = sev;
        diag.confidence = confidence;
        diag.evidenceTier = hasAtomicPairs ? EvidenceTier::Proven : EvidenceTier::Likely;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << RD->getNameAsString() << "' ("
           << map.recordSizeBytes() << "B, "
           << map.linesSpanned() << " line(s)): "
           << mutablePairs.size() << " mutable field pair(s) share cache line(s) "
           << "with thread-escape evidence. Concurrent writes to co-located "
           << "fields trigger MESI invalidation per write.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "sizeof=" << map.recordSizeBytes() << "B"
           << "; lines=" << map.linesSpanned()
           << "; mutable_pairs_same_line=" << mutablePairs.size()
           << "; atomic_pairs_same_line=" << map.atomicPairsOnSameLine().size()
           << "; thread_escape=true"
           << "; atomics=" << (map.totalAtomicFields() > 0 ? "yes" : "no");
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Pad independently-written fields to separate 64B cache lines "
            "with alignas(64). Consider per-thread/per-core replicas.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL002_FalseSharing)

} // namespace faultline
