#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/analysis/EscapeAnalysis.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

class FL060_NUMAUnfriendly : public Rule {
public:
    std::string_view getID() const override { return "FL060"; }
    std::string_view getTitle() const override { return "NUMA-Unfriendly Shared Structure"; }
    Severity getBaseSeverity() const override { return Severity::High; }

    std::string_view getHardwareMechanism() const override {
        return "On multi-socket systems, memory is physically partitioned across "
               "NUMA nodes. Accessing remote memory incurs ~100-300ns penalty vs "
               "~60-80ns local. Large shared mutable structures allocated without "
               "NUMA-aware placement will be accessed remotely by at least one socket.";
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

        const auto &layout = Ctx.getASTRecordLayout(RD);
        uint64_t sizeBytes = layout.getSize().getQuantity();

        // NUMA risk is significant for structures that:
        // 1. Are large enough to span multiple cache lines (>= 256B)
        // 2. Show thread-escape evidence
        // 3. Contain mutable state
        if (sizeBytes < 256)
            return;

        EscapeAnalysis escape(Ctx);

        if (!escape.mayEscapeThread(RD))
            return;

        unsigned mutableCount = 0;
        bool hasAtomics = escape.hasAtomicMembers(RD);

        for (const auto *field : RD->fields()) {
            if (escape.isFieldMutable(field))
                ++mutableCount;
        }

        if (mutableCount == 0 && !hasAtomics)
            return;

        Severity sev = Severity::High;
        std::vector<std::string> escalations;

        uint64_t cacheLines = (sizeBytes + Cfg.cacheLineBytes - 1) / Cfg.cacheLineBytes;

        if (sizeBytes >= 4096) {
            sev = Severity::Critical;
            escalations.push_back(
                "sizeof >= 4KB: spans " + std::to_string(cacheLines) +
                " cache lines, guaranteed multi-page TLB footprint on "
                "remote NUMA node");
        }

        if (hasAtomics) {
            escalations.push_back(
                "Contains atomic fields: cross-socket atomic RMW incurs "
                "interconnect round-trip (~200-400ns on QPI/UPI)");
        }

        if (mutableCount > 8) {
            escalations.push_back(
                std::to_string(mutableCount) + " mutable fields: wide "
                "write surface amplifies remote store buffer pressure");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL060";
        diag.title     = "NUMA-Unfriendly Shared Structure";
        diag.severity  = sev;
        diag.confidence = hasAtomics ? 0.55 : 0.35;
        diag.evidenceTier = EvidenceTier::Speculative;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << RD->getNameAsString() << "' (" << sizeBytes
           << "B, " << cacheLines << " cache lines) with "
           << mutableCount << " mutable field(s) and thread-escape evidence. "
           << "On multi-socket systems, at least one socket accesses this "
           << "structure via remote NUMA interconnect. Each remote cache line "
           << "fetch adds ~100-300ns. Atomic operations on remote lines "
           << "require interconnect round-trip.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "struct=" << RD->getNameAsString()
           << "; sizeof=" << sizeBytes << "B"
           << "; cache_lines=" << cacheLines
           << "; mutable_fields=" << mutableCount
           << "; atomics=" << (hasAtomics ? "yes" : "no")
           << "; thread_escape=yes";
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Use numa_alloc_onnode() or mbind() for NUMA-aware placement. "
            "Replicate structure per-socket with periodic synchronization. "
            "Split into read-mostly (replicated) and write-heavy (local) parts. "
            "Consider interleaved allocation for balanced access patterns.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL060_NUMAUnfriendly)

} // namespace faultline
