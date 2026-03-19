// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/LayoutSafety.h"
#include "lshaz/core/Rule.h"
#include "lshaz/core/RuleRegistry.h"
#include "lshaz/core/HotPathOracle.h"
#include "lshaz/core/Config.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Type.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>

namespace lshaz {

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
                 const Config &Cfg,
                 EscapeAnalysis & /*Escape*/,
                 std::vector<Diagnostic> &out) override {

        const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return;

        // Estimate stack frame size from all local variable declarations,
        // including those inside nested blocks, loops, and conditionals.
        uint64_t totalBytes = 0;
        std::vector<std::pair<std::string, uint64_t>> largeLocals;

        const auto *body = FD->getBody();
        if (!body)
            return;

        class LocalVarVisitor
            : public clang::RecursiveASTVisitor<LocalVarVisitor> {
        public:
            clang::ASTContext &ctx;
            uint64_t &total;
            std::vector<std::pair<std::string, uint64_t>> &large;
            LocalVarVisitor(clang::ASTContext &c, uint64_t &t,
                            std::vector<std::pair<std::string, uint64_t>> &l)
                : ctx(c), total(t), large(l) {}

            bool VisitVarDecl(clang::VarDecl *VD) {
                if (!VD->hasLocalStorage())
                    return true;
                clang::QualType QT = VD->getType();
                if (!canComputeTypeSize(QT, ctx))
                    return true;
                uint64_t sz = ctx.getTypeSizeInChars(QT).getQuantity();
                total += sz;
                if (sz >= 256)
                    large.push_back({VD->getNameAsString(), sz});
                return true;
            }
        };

        LocalVarVisitor localVisitor(Ctx, totalBytes, largeLocals);
        localVisitor.TraverseStmt(const_cast<clang::Stmt *>(body));

        // Also account for parameters passed by value.
        for (const auto *param : FD->parameters()) {
            clang::QualType QT = param->getType();
            if (!QT->isReferenceType() && !QT->isPointerType()) {
                if (!canComputeTypeSize(QT, Ctx))
                    continue;
                uint64_t sz = Ctx.getTypeSizeInChars(QT).getQuantity();
                totalBytes += sz;
            }
        }

        const uint64_t threshold = Cfg.stackFrameWarnBytes;

        if (totalBytes < threshold)
            return;

        bool isHot = Oracle.isFunctionHot(FD);
        Severity sev = isHot ? Severity::High : Severity::Medium;
        std::vector<std::string> escalations;

        if (totalBytes > Cfg.pageSize) {
            escalations.push_back(
                "Stack frame exceeds page size (" +
                std::to_string(Cfg.pageSize / 1024) +
                "KB): guaranteed TLB miss on first access, potential page fault");
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
        diag.functionName = FD->getQualifiedNameAsString();

        diag.location = resolveSourceLocation(loc, SM);

        std::ostringstream hw;
        hw << "Function '" << FD->getQualifiedNameAsString()
           << "' estimated stack frame ~" << totalBytes
           << "B. Spans ~" << ((totalBytes + Cfg.pageSize - 1) / Cfg.pageSize)
           << " page(s). Large stack frames increase D-TLB working set, "
           << "pressure L1D capacity, and risk stack page faults "
           << "on deep call chains.";
        diag.hardwareReasoning = hw.str();

        diag.structuralEvidence = {
            {"estimated_frame", std::to_string(totalBytes) + "B"},
            {"threshold", std::to_string(threshold) + "B"},
        };
        if (!largeLocals.empty()) {
            std::string locals;
            for (size_t i = 0; i < largeLocals.size(); ++i) {
                locals += largeLocals[i].first + "(" +
                          std::to_string(largeLocals[i].second) + "B)";
                if (i + 1 < largeLocals.size()) locals += ", ";
            }
            diag.structuralEvidence["large_locals"] = locals;
        }

        diag.mitigation =
            "Move large arrays to heap with arena allocator. "
            "Use static/thread_local buffers for fixed-size data. "
            "Reduce local buffer sizes. "
            "Consider passing large structures by reference.";

        diag.escalations = std::move(escalations);
        out.push_back(std::move(diag));
    }
};

LSHAZ_REGISTER_RULE(FL021_LargeStackFrame)

} // namespace lshaz
