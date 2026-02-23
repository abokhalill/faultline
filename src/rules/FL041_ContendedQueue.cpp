#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/analysis/CacheLineMap.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

class FL041_ContendedQueue : public Rule {
public:
    std::string_view getID() const override { return "FL041"; }
    std::string_view getTitle() const override { return "Contended Queue Pattern"; }
    Severity getBaseSeverity() const override { return Severity::High; }

    std::string_view getHardwareMechanism() const override {
        return "Head/tail index cache line bouncing in MPMC queues. "
               "Atomic head and tail on same cache line causes MESI "
               "invalidation on every enqueue/dequeue from different cores. "
               "Without padding, producer and consumer thrash the same line.";
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

        CacheLineMap map(RD, Ctx);

        auto atomicPairs = map.atomicPairsOnSameLine();
        if (atomicPairs.empty())
            return;

        const auto &firstPair = atomicPairs.front();
        std::string field1 = firstPair.a->name;
        std::string field2 = firstPair.b->name;

        Severity sev = Severity::High;
        std::vector<std::string> escalations;

        std::string structName = RD->getNameAsString();
        bool looksLikeQueue =
            structName.find("queue") != std::string::npos ||
            structName.find("Queue") != std::string::npos ||
            structName.find("buffer") != std::string::npos ||
            structName.find("Buffer") != std::string::npos ||
            structName.find("ring") != std::string::npos ||
            structName.find("Ring") != std::string::npos;

        bool hasHeadTail = false;
        for (const auto &f : map.fields()) {
            if (!f.isAtomic) continue;
            const auto &n = f.name;
            if (n.find("head") != std::string::npos ||
                n.find("tail") != std::string::npos ||
                n.find("read") != std::string::npos ||
                n.find("write") != std::string::npos ||
                n.find("push") != std::string::npos ||
                n.find("pop") != std::string::npos ||
                n.find("front") != std::string::npos ||
                n.find("back") != std::string::npos) {
                hasHeadTail = true;
            }
        }

        if (looksLikeQueue || hasHeadTail) {
            sev = Severity::Critical;
            escalations.push_back(
                "Structure appears to be a concurrent queue: head/tail "
                "atomic indices on same cache line guarantee producer-consumer "
                "cache line ping-pong");
        }

        for (const auto &pair : atomicPairs) {
            escalations.push_back(
                "atomic fields '" + pair.a->name + "' and '" + pair.b->name +
                "' share line " + std::to_string(pair.lineIndex) +
                ": concurrent writes trigger MESI invalidation");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL041";
        diag.title     = "Contended Queue Pattern";
        diag.severity  = sev;
        diag.confidence = (looksLikeQueue || hasHeadTail) ? 0.82 : 0.62;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << structName << "' ("
           << map.recordSizeBytes() << "B, "
           << map.linesSpanned() << " line(s)) has "
           << map.totalAtomicFields() << " atomic field(s) with '"
           << field1 << "' and '" << field2
           << "' on the same cache line. Under MPMC workload, every "
           << "enqueue/dequeue triggers cross-core RFO for the shared line.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "struct=" << structName
           << "; sizeof=" << map.recordSizeBytes() << "B"
           << "; lines=" << map.linesSpanned()
           << "; atomic_fields=" << map.totalAtomicFields()
           << "; atomic_pairs_same_line=" << atomicPairs.size()
           << "; queue_heuristic=" << (looksLikeQueue ? "yes" : "no")
           << "; head_tail_names=" << (hasHeadTail ? "yes" : "no");
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Pad head and tail indices to separate 64B cache lines using "
            "alignas(64). Use per-core queues (SPSC) where possible. "
            "Consider cache-line-aware queue implementations.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL041_ContendedQueue)

} // namespace faultline
