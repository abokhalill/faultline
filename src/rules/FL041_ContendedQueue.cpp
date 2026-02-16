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

        // Heuristic: look for structs with 2+ atomic integer fields
        // that look like head/tail indices (common queue pattern).
        EscapeAnalysis escape(Ctx);

        struct AtomicField {
            const clang::FieldDecl *decl;
            uint64_t offsetBits;
        };

        std::vector<AtomicField> atomicFields;
        const auto &layout = Ctx.getASTRecordLayout(RD);

        unsigned idx = 0;
        for (const auto *field : RD->fields()) {
            if (escape.isAtomicType(field->getType())) {
                uint64_t offset = layout.getFieldOffset(idx);
                atomicFields.push_back({field, offset});
            }
            ++idx;
        }

        // Need at least 2 atomic fields for queue contention pattern.
        if (atomicFields.size() < 2)
            return;

        // Check if any pair of atomic fields share a cache line (64B = 512 bits).
        bool hasSameLineAtomics = false;
        std::string field1, field2;

        for (size_t i = 0; i < atomicFields.size(); ++i) {
            for (size_t j = i + 1; j < atomicFields.size(); ++j) {
                uint64_t line_i = atomicFields[i].offsetBits / 512;
                uint64_t line_j = atomicFields[j].offsetBits / 512;
                if (line_i == line_j) {
                    hasSameLineAtomics = true;
                    field1 = atomicFields[i].decl->getNameAsString();
                    field2 = atomicFields[j].decl->getNameAsString();
                    break;
                }
            }
            if (hasSameLineAtomics) break;
        }

        if (!hasSameLineAtomics)
            return;

        Severity sev = Severity::High;
        std::vector<std::string> escalations;

        // Check for queue-like naming heuristic.
        std::string structName = RD->getNameAsString();
        bool looksLikeQueue =
            structName.find("queue") != std::string::npos ||
            structName.find("Queue") != std::string::npos ||
            structName.find("buffer") != std::string::npos ||
            structName.find("Buffer") != std::string::npos ||
            structName.find("ring") != std::string::npos ||
            structName.find("Ring") != std::string::npos;

        // Check field names for head/tail pattern.
        bool hasHeadTail = false;
        for (const auto &af : atomicFields) {
            std::string name = af.decl->getNameAsString();
            if (name.find("head") != std::string::npos ||
                name.find("tail") != std::string::npos ||
                name.find("read") != std::string::npos ||
                name.find("write") != std::string::npos ||
                name.find("push") != std::string::npos ||
                name.find("pop") != std::string::npos ||
                name.find("front") != std::string::npos ||
                name.find("back") != std::string::npos) {
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

        escalations.push_back(
            "Atomic fields '" + field1 + "' and '" + field2 +
            "' share a cache line: concurrent writes from different cores "
            "will trigger MESI invalidation on every operation");

        const auto &SM = Ctx.getSourceManager();
        auto loc = RD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL041";
        diag.title     = "Contended Queue Pattern";
        diag.severity  = sev;
        diag.confidence = (looksLikeQueue || hasHeadTail) ? 0.80 : 0.60;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        uint64_t sizeBytes = layout.getSize().getQuantity();

        std::ostringstream hw;
        hw << "Struct '" << structName << "' (" << sizeBytes
           << "B) has " << atomicFields.size()
           << " atomic fields with '" << field1 << "' and '" << field2
           << "' on the same cache line. Under MPMC workload, every "
           << "enqueue/dequeue triggers cross-core RFO for the shared line.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "struct=" << structName
           << "; sizeof=" << sizeBytes << "B"
           << "; atomic_fields=" << atomicFields.size()
           << "; same_line_pair=[" << field1 << ", " << field2 << "]"
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
