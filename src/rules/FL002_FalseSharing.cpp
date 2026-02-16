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

        const auto &layout = Ctx.getASTRecordLayout(RD);
        uint64_t sizeBytes = layout.getSize().getQuantity();

        // False sharing is relevant when struct fits within or near a single
        // cache line — multiple fields contend on the same line.
        // Structs > 128B are FL001 territory.
        if (sizeBytes > 128)
            return;

        EscapeAnalysis escape(Ctx);

        // Count mutable non-const fields.
        unsigned mutableCount = 0;
        bool hasAtomics = false;
        std::vector<std::string> mutableFields;

        for (const auto *field : RD->fields()) {
            if (escape.isFieldMutable(field)) {
                ++mutableCount;
                mutableFields.push_back(field->getNameAsString());
            }
            if (escape.isAtomicType(field->getType())) {
                hasAtomics = true;
            }
        }

        // Need 2+ mutable fields for false sharing risk.
        if (mutableCount < 2)
            return;

        // Must show evidence of cross-thread usage.
        if (!escape.mayEscapeThread(RD))
            return;

        Severity sev = Severity::Critical;
        std::vector<std::string> escalations;

        if (hasAtomics) {
            escalations.push_back(
                "Contains std::atomic fields: guaranteed cross-core "
                "cache line invalidation on write");
        }

        // Check if fields that should be on separate lines are packed together.
        // If struct <= 64B, all mutable fields share a single cache line.
        if (sizeBytes <= 64) {
            escalations.push_back(
                "All mutable fields packed within single 64B cache line: "
                "maximum invalidation surface");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL002";
        diag.title     = "False Sharing Candidate";
        diag.severity  = sev;
        diag.confidence = hasAtomics ? 0.85 : 0.65;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Struct '" << RD->getNameAsString() << "' (" << sizeBytes
           << "B) contains " << mutableCount
           << " mutable fields with cross-thread escape evidence. "
           << "Concurrent writes to different fields will trigger MESI "
           << "invalidation storms on the shared cache line.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "sizeof=" << sizeBytes << "B; mutable_fields=[";
        for (size_t i = 0; i < mutableFields.size(); ++i) {
            ev << mutableFields[i];
            if (i + 1 < mutableFields.size()) ev << ", ";
        }
        ev << "]; thread_escape=true; atomics=" << (hasAtomics ? "yes" : "no");
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Pad fields to separate 64B cache lines. "
            "Use alignas(64) on independently-written fields. "
            "Consider per-thread/per-core storage.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }

private:
    bool isAtomicType(clang::QualType QT) const {
        QT = QT.getCanonicalType();
        if (QT.getAsString().find("atomic") != std::string::npos)
            return true;
        return QT->isAtomicType();
    }
};

FAULTLINE_REGISTER_RULE(FL002_FalseSharing)

} // namespace faultline
