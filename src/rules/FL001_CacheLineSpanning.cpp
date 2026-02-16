#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/core/Config.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

class FL001_CacheLineSpanning : public Rule {
public:
    std::string_view getID() const override { return "FL001"; }
    std::string_view getTitle() const override { return "Cache Line Spanning Struct"; }
    Severity getBaseSeverity() const override { return Severity::High; }

    std::string_view getHardwareMechanism() const override {
        return "L1/L2 cache line footprint expansion. Increased eviction "
               "probability. Higher coherence traffic under multi-core writes.";
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

        // Threshold: struct exceeds single 64B cache line.
        if (sizeBytes <= 64)
            return;

        Severity sev = Severity::High;
        std::vector<std::string> escalations;

        if (sizeBytes > 128) {
            sev = Severity::Critical;
            escalations.push_back(
                "sizeof > 128B: spans 3+ cache lines, elevated eviction pressure");
        }

        // Check for atomic fields — escalation per RULEBOOK.
        bool hasAtomics = false;
        for (const auto *field : RD->fields()) {
            auto qt = field->getType().getCanonicalType();
            if (qt.getAsString().find("atomic") != std::string::npos) {
                hasAtomics = true;
                break;
            }
        }

        if (hasAtomics) {
            sev = Severity::Critical;
            escalations.push_back(
                "Contains atomic fields: coherence traffic amplified across "
                "spanned cache lines (MESI RFO storms)");
        }

        // Build diagnostic.
        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL001";
        diag.title     = "Cache Line Spanning Struct";
        diag.severity  = sev;
        diag.confidence = hasAtomics ? 0.90 : 0.75;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << RD->getNameAsString() << "' occupies "
           << sizeBytes << "B, spanning "
           << ((sizeBytes + 63) / 64) << " cache line(s). "
           << "Each access may touch multiple lines, increasing "
           << "L1D pressure and coherence invalidation surface.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "sizeof(" << RD->getNameAsString() << ") = " << sizeBytes
           << "B; cache_line = 64B; lines_spanned = "
           << ((sizeBytes + 63) / 64);
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Split hot/cold fields into separate structs. "
            "Consider AoS->SoA transformation. "
            "Apply alignas(64) to isolate write-heavy sub-structs.";

        diag.escalations = std::move(escalations);

        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL001_CacheLineSpanning)

} // namespace faultline
