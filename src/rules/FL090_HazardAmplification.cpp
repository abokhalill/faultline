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

class FL090_HazardAmplification : public Rule {
public:
    std::string_view getID() const override { return "FL090"; }
    std::string_view getTitle() const override { return "Hazard Amplification"; }
    Severity getBaseSeverity() const override { return Severity::Critical; }

    std::string_view getHardwareMechanism() const override {
        return "Multiple interacting latency multipliers on a single structure: "
               "cache line spanning + atomic contention + cross-thread sharing. "
               "Each hazard compounds under load. Coherence storms, store buffer "
               "saturation, and TLB pressure interact to produce tail latency.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle & /*Oracle*/,
                 const Config &Cfg,
                 std::vector<Diagnostic> &out) override {

        const auto *RD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(D);
        if (!RD || !RD->isCompleteDefinition())
            return;
        if (RD->isImplicit() || RD->isLambda())
            return;

        CacheLineMap map(RD, Ctx, Cfg.cacheLineBytes);
        EscapeAnalysis escape(Ctx);

        bool multiLine    = map.linesSpanned() >= 3;
        bool hasAtomics   = map.totalAtomicFields() > 0;
        bool threadEscape = escape.mayEscapeThread(RD);

        unsigned signalCount = 0;
        if (multiLine)    ++signalCount;
        if (hasAtomics)   ++signalCount;
        if (threadEscape) ++signalCount;

        if (signalCount < 3)
            return;

        Severity sev = Severity::Critical;
        std::vector<std::string> escalations;

        unsigned atomicLines = 0;
        unsigned hotLines = 0;
        for (const auto &b : map.buckets()) {
            if (b.atomicCount > 0) ++atomicLines;
            if (b.mutableCount > 0) ++hotLines;
        }

        escalations.push_back(
            std::to_string(map.recordSizeBytes()) + "B across " +
            std::to_string(map.linesSpanned()) + " cache lines");

        escalations.push_back(
            std::to_string(map.totalAtomicFields()) + " atomic field(s) on " +
            std::to_string(atomicLines) + " line(s): per-line RFO ownership transfer");

        escalations.push_back(
            "thread-escaping: coherence traffic amplified across participating cores");

        auto straddlers = map.straddlingFields();
        if (!straddlers.empty()) {
            escalations.push_back(
                std::to_string(straddlers.size()) +
                " field(s) straddle line boundaries: split load/store penalty "
                "compounds with coherence cost");
        }

        if (map.totalMutableFields() > 4) {
            escalations.push_back(
                std::to_string(map.totalMutableFields()) + " mutable fields across " +
                std::to_string(hotLines) + " line(s): wide write surface");
        }

        auto atomicPairs = map.atomicPairsOnSameLine();
        if (!atomicPairs.empty()) {
            escalations.push_back(
                std::to_string(atomicPairs.size()) +
                " atomic pair(s) share cache line(s): intra-line contention "
                "adds to cross-line RFO cost");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL090";
        diag.title     = "Hazard Amplification";
        diag.severity  = sev;
        diag.confidence = 0.88;
        diag.evidenceTier = EvidenceTier::Likely;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << RD->getNameAsString() << "' ("
           << map.recordSizeBytes() << "B, "
           << map.linesSpanned() << " lines) exhibits compound hazard: "
           << map.totalAtomicFields() << " atomic field(s) across "
           << atomicLines << " line(s) with thread-escape evidence. "
           << "Under multi-core contention, per-line RFO ownership transfer "
           << "and coherence invalidation interact across the full footprint.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "struct=" << RD->getNameAsString()
           << "; sizeof=" << map.recordSizeBytes() << "B"
           << "; cache_lines=" << map.linesSpanned()
           << "; atomic_fields=" << map.totalAtomicFields()
           << "; atomic_lines=" << atomicLines
           << "; mutable_fields=" << map.totalMutableFields()
           << "; straddling=" << straddlers.size()
           << "; thread_escape=yes"
           << "; signal_count=" << signalCount;
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Decompose into separate cache-line-aligned sub-structures. "
            "Isolate atomic fields with alignas(64) padding. "
            "Split hot (frequently written) and cold (rarely accessed) fields. "
            "Consider per-core replicas with periodic merge.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL090_HazardAmplification)

} // namespace faultline
