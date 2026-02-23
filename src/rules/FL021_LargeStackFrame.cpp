#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"
#include "faultline/core/Config.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace faultline {

class FL021_LargeStackFrame : public Rule {
public:
    std::string_view getID() const override { return "FL021"; }
    std::string_view getTitle() const override { return "Large Stack Frame"; }
    Severity getBaseSeverity() const override { return Severity::Medium; }

    std::string_view getHardwareMechanism() const override {
        return "TLB pressure from stack spanning multiple pages. "
               "L1D cache pressure from large working set. "
               "Potential stack page faults on deep call chains.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle &Oracle,
                 std::vector<Diagnostic> &out) override {

        const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return;

        // Estimate stack frame size from local variable declarations.
        uint64_t totalBytes = 0;
        std::vector<std::pair<std::string, uint64_t>> largeLocals;

        const auto *body = FD->getBody();
        if (!body)
            return;

        for (const auto *child : body->children()) {
            if (const auto *DS = llvm::dyn_cast<clang::DeclStmt>(child)) {
                for (const auto *decl : DS->decls()) {
                    if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(decl)) {
                        if (!VD->hasLocalStorage())
                            continue;
                        clang::QualType QT = VD->getType();
                        if (QT->isIncompleteType() || QT->isDependentType())
                            continue;

                        uint64_t sz = Ctx.getTypeSizeInChars(QT).getQuantity();
                        totalBytes += sz;

                        if (sz >= 256)
                            largeLocals.push_back({VD->getNameAsString(), sz});
                    }
                }
            }
        }

        // Also account for parameters passed by value.
        for (const auto *param : FD->parameters()) {
            clang::QualType QT = param->getType();
            if (QT->isIncompleteType() || QT->isDependentType())
                continue;
            if (!QT->isReferenceType() && !QT->isPointerType()) {
                uint64_t sz = Ctx.getTypeSizeInChars(QT).getQuantity();
                totalBytes += sz;
            }
        }

        const uint64_t threshold = 2048; // 2KB default per RULEBOOK

        if (totalBytes < threshold)
            return;

        bool isHot = Oracle.isFunctionHot(FD);
        Severity sev = isHot ? Severity::High : Severity::Medium;
        std::vector<std::string> escalations;

        if (totalBytes > 4096) {
            escalations.push_back(
                "Stack frame exceeds page size (4KB): guaranteed TLB miss "
                "on first access, potential page fault");
            if (isHot)
                sev = Severity::Critical;
        }

        if (isHot) {
            escalations.push_back("Function is on hot path");
        }

        const auto &SM = Ctx.getSourceManager();
        auto loc = FD->getLocation();

        Diagnostic diag;
        diag.ruleID    = "FL021";
        diag.title     = "Large Stack Frame";
        diag.severity  = sev;
        diag.confidence = 0.80;
        diag.evidenceTier = EvidenceTier::Likely;

        if (loc.isValid()) {
            diag.location.file   = SM.getFilename(SM.getSpellingLoc(loc)).str();
            diag.location.line   = SM.getSpellingLineNumber(loc);
            diag.location.column = SM.getSpellingColumnNumber(loc);
        }

        std::ostringstream hw;
        hw << "Function '" << FD->getQualifiedNameAsString()
           << "' estimated stack frame ~" << totalBytes
           << "B. Spans ~" << ((totalBytes + 4095) / 4096)
           << " page(s). Large stack frames increase D-TLB working set, "
           << "pressure L1D capacity, and risk stack page faults "
           << "on deep call chains.";
        diag.hardwareReasoning = hw.str();

        std::ostringstream ev;
        ev << "estimated_frame=" << totalBytes << "B; threshold=" << threshold << "B";
        if (!largeLocals.empty()) {
            ev << "; large_locals=[";
            for (size_t i = 0; i < largeLocals.size(); ++i) {
                ev << largeLocals[i].first << "(" << largeLocals[i].second << "B)";
                if (i + 1 < largeLocals.size()) ev << ", ";
            }
            ev << "]";
        }
        diag.structuralEvidence = ev.str();

        diag.mitigation =
            "Move large arrays to heap with arena allocator. "
            "Use static/thread_local buffers for fixed-size data. "
            "Reduce local buffer sizes. "
            "Consider passing large structures by reference.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

FAULTLINE_REGISTER_RULE(FL021_LargeStackFrame)

} // namespace faultline
