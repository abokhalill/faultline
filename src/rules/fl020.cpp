// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/rule.h"
#include "lshaz/core/registry.h"
#include "lshaz/core/hot_path.h"
#include "lshaz/analysis/allocator.h"
#include "lshaz/analysis/data_flow.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>
#include <string>

namespace lshaz {

namespace {

struct AllocSite {
    clang::SourceLocation loc;
    std::string kind; // "new", "delete", "malloc", "std::make_shared", etc.
    unsigned inLoop = 0;
    AllocatorClass allocClass = AllocatorClass::Unknown;
};

class AllocVisitor : public clang::RecursiveASTVisitor<AllocVisitor> {
public:
    explicit AllocVisitor(clang::ASTContext &Ctx) : ctx_(Ctx) {}

    bool VisitCXXNewExpr(clang::CXXNewExpr *E) {
        sites_.push_back({E->getBeginLoc(), "operator new", inLoop_});
        return true;
    }

    bool VisitCXXDeleteExpr(clang::CXXDeleteExpr *E) {
        sites_.push_back({E->getBeginLoc(), "operator delete", inLoop_});
        return true;
    }

    bool VisitCallExpr(clang::CallExpr *E) {
        if (const auto *callee = E->getDirectCallee()) {
            std::string name = callee->getQualifiedNameAsString();

            if (name == "malloc" || name == "calloc" || name == "realloc" ||
                name == "free" || name == "aligned_alloc" ||
                name == "posix_memalign") {
                sites_.push_back({E->getBeginLoc(), name, inLoop_});
            }

            if (name == "std::make_shared" || name == "std::make_unique" ||
                name == "std::make_shared_for_overwrite" ||
                name == "std::make_unique_for_overwrite") {
                sites_.push_back({E->getBeginLoc(), name, inLoop_});
            }
        }
        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr *E) {
        if (const auto *CD = E->getConstructor()) {
            // Resolve template name through ClassTemplateSpecializationDecl.
            std::string templateName;
            if (const auto *CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                    CD->getParent())) {
                if (auto *TD = CTSD->getSpecializedTemplate())
                    templateName = TD->getQualifiedNameAsString();
            }
            if (templateName.empty())
                templateName = CD->getParent()->getQualifiedNameAsString();

            static const char *heapAllocTypes[] = {
                "std::function",
                "std::shared_ptr",
                "std::vector", "std::map", "std::unordered_map",
                "std::list", "std::deque", "std::set", "std::unordered_set",
                "std::basic_string"
            };

            for (const auto *ht : heapAllocTypes) {
                if (templateName == ht) {
                    std::string label = std::string(ht) + " ctor";
                    sites_.push_back({E->getBeginLoc(), label, inLoop_});
                    break;
                }
            }

            // libstdc++ mangled string path.
            if (templateName == "std::__cxx11::basic_string")
                sites_.push_back({E->getBeginLoc(), "std::string ctor", inLoop_});
        }
        return true;
    }

    // Track loop nesting for escalation.
    bool TraverseForStmt(clang::ForStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AllocVisitor>::TraverseForStmt(S);
        --inLoop_;
        return r;
    }

    bool TraverseWhileStmt(clang::WhileStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AllocVisitor>::TraverseWhileStmt(S);
        --inLoop_;
        return r;
    }

    bool TraverseDoStmt(clang::DoStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AllocVisitor>::TraverseDoStmt(S);
        --inLoop_;
        return r;
    }

    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<AllocVisitor>::TraverseCXXForRangeStmt(S);
        --inLoop_;
        return r;
    }

    const std::vector<AllocSite> &sites() const { return sites_; }

private:
    clang::ASTContext &ctx_;
    std::vector<AllocSite> sites_;
    unsigned inLoop_ = 0;
};

} // anonymous namespace

class FL020_HeapAllocHotPath : public Rule {
public:
    std::string_view getID() const override { return "FL020"; }
    std::string_view getTitle() const override { return "Heap Allocation in Hot Path"; }
    Severity getBaseSeverity() const override { return Severity::Critical; }

    bool requiresHotPath() const override { return true; }

    unsigned requiredFeatures() const override { return FEAT_CALL; }

    std::string_view getHardwareMechanism() const override {
        return "The allocate/free round trip itself: 14.7ns at 64B, 55ns at "
               "4KB, always paid. Arena lock contention is not a general cost "
               "— same-thread alloc/free measures flat from 1 to 3 threads, "
               "because tcache and per-thread arenas keep it off shared state. "
               "It appears when a block is freed by a thread other than the "
               "one that allocated it, returning it to the owning arena: 5-25x "
               "under glibc, still 4-5x under jemalloc. Volume is not the "
               "signal; cross-thread ownership transfer is.";
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

        if (!Oracle.isFunctionHot(FD))
            return;

        AllocVisitor visitor(Ctx);
        visitor.TraverseStmt(FD->getBody());

        // Intra-procedural data-flow analysis for precision.
        DataFlowAnalyzer dfa(Ctx);
        DataFlowFacts dfFacts = dfa.analyze(FD);

        // Classify allocation sites by allocator topology.
        AllocatorTopology topo;
        if (!Cfg.linkedAllocator.empty())
            topo.setLinkedAllocator(Cfg.linkedAllocator);

        const auto &SM = Ctx.getSourceManager();

        for (const auto &site : visitor.sites()) {
            AllocatorClass ac = topo.classify(site.kind);
            double sevFactor = allocatorSeverityFactor(ac);

            // Base severity modulated by allocator topology.
            Severity sev = Severity::Critical;
            if (sevFactor < 0.4)
                sev = Severity::Medium;
            else if (sevFactor < 0.8)
                sev = Severity::High;

            double confidence = 0.75 * sevFactor;
            if (confidence < 0.20)
                confidence = 0.20;
            if (confidence > 1.0)
                confidence = 1.0;

            std::vector<std::string> escalations;

            if (site.inLoop) {
                escalations.push_back(
                    "Allocation inside loop: per-iteration allocator pressure, "
                    "compounding TLB and fragmentation cost");
                // Loop escalation overrides topology demotion.
                if (sev < Severity::High)
                    sev = Severity::High;
            }

            if (ac == AllocatorClass::ThreadLocal) {
                escalations.push_back(
                    "allocator-topology: thread-local cache path (" +
                    Cfg.linkedAllocator + "), reduced contention risk");
            } else if (ac == AllocatorClass::PoolSlab) {
                escalations.push_back(
                    "allocator-topology: pool/slab allocator, minimal latency");
            } else if (ac == AllocatorClass::Syscall) {
                escalations.push_back(
                    "allocator-topology: mmap/brk syscall path, page fault risk");
            }

            Diagnostic diag;
            diag.ruleID    = "FL020";
            diag.title     = "Heap Allocation in Hot Path";
            diag.severity  = sev;
            diag.confidence = confidence;
            diag.evidenceTier = EvidenceTier::Likely;
            diag.functionName = FD->getQualifiedNameAsString();

            diag.location = resolveSourceLocation(site.loc, SM);

            std::ostringstream hw;
            hw << "'" << site.kind << "' in hot function '"
               << FD->getQualifiedNameAsString()
               << "'. Allocator class: " << allocatorClassName(ac)
               << ". ";
            if (ac == AllocatorClass::ThreadLocal) {
                // A thread-cache hit returns a previously freed chunk from a
                // per-thread free list: no lock is taken and no page is
                // mapped, so neither arena contention nor TLB pressure
                // applies. What remains is the call itself and the locality
                // loss against inline or stack storage.
                hw << "Thread-local cache hit expected (" << Cfg.linkedAllocator
                   << "): no arena lock and no new page mapping, so neither "
                   << "contention nor TLB reach is at issue here. The cost is "
                   << "the allocate/free round trip and the loss of locality "
                   << "against inline or stack storage — the returned chunk "
                   << "is typically cold in L1D.";
            } else if (ac == AllocatorClass::Syscall) {
                hw << "Large allocation triggers mmap syscall. "
                   << "Page fault jitter, TLB shootdown on munmap.";
            } else {
                hw << "May contend on allocator arena locks, "
                   << "trigger mmap/brk syscalls, fault new pages into the TLB, "
                   << "and fragment the heap reducing spatial locality.";
            }
            hw << " [Assumes: allocation frequency is high at runtime on this path]";
            diag.hardwareReasoning = hw.str();

            // Data-flow escalations.
            bool escapes = false;
            bool flowsToLoop = false;
            if (diag.location.line > 0) {
                escapes = dfFacts.allocEscapes.count(diag.location.line) > 0;
                flowsToLoop = dfFacts.allocFlowsToLoop.count(diag.location.line) > 0;
            }
            if (escapes) {
                escalations.push_back(
                    "data-flow: allocated pointer escapes function "
                    "(passed to callee, stored to field, or returned)");
                if (confidence < 0.85)
                    confidence = std::min(confidence + 0.10, 1.0);
            }
            if (flowsToLoop && !site.inLoop) {
                escalations.push_back(
                    "data-flow: allocation result flows into loop body");
                if (sev < Severity::High)
                    sev = Severity::High;
            }

            diag.severity = sev;
            diag.confidence = confidence;

            diag.structuralEvidence = {
                {"alloc_type", site.kind},
                {"allocator_class", std::string(allocatorClassName(ac))},
                {"function", FD->getQualifiedNameAsString()},
                {"in_loop", site.inLoop ? "yes" : "no"},
                {"hot_path", "true"},
                {"alloc_escapes", escapes ? "yes" : "no"},
                {"flows_to_loop", flowsToLoop ? "yes" : "no"},
            };

            diag.mitigation =
                "Preallocate buffers. Use arena/slab/pool allocators. "
                "Move allocation to cold initialization path. "
                "Reserve std::vector capacity upfront.";

            diag.escalations = std::move(escalations);
            diag.mechanismClaims = {
                {"allocate/free round trip, and locality lost against inline "
                 "or stack storage",
                 "an allocation on a hot path", true, Severity::Medium},
                // Measured flat 1->3 threads for same-thread alloc/free, so
                // allocator class alone cannot establish contention.
                {"allocator arena lock contention",
                 "a free on a thread other than the allocating one, which "
                 "returns the block to another thread's arena",
                 false, Severity::High},
                {"mmap syscall, page faults, TLB shootdown on munmap",
                 "an allocation above the mmap threshold",
                 ac == AllocatorClass::Syscall, Severity::Critical},
                {"the cost is paid on every iteration",
                 "the allocation sits inside a loop", site.inLoop != 0,
                 Severity::High},
                // Cross-thread free is the only term that reaches Critical,
                // and proving it needs alloc/free ownership across TUs, which
                // we cannot do yet. Unestablished so the grade says so.
                {"the block is freed by a thread other than the allocator, "
                 "costing 4-5x under jemalloc and up to 25x under glibc",
                 "alloc and free attributed to disjoint thread roles",
                 false, Severity::High, /*gating=*/true},
            };
            out.push_back(std::move(diag));
        }
    }
};

LSHAZ_REGISTER_RULE(FL020_HeapAllocHotPath)

} // namespace lshaz
