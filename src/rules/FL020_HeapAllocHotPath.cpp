#include "faultline/core/Rule.h"
#include "faultline/core/RuleRegistry.h"
#include "faultline/core/HotPathOracle.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceManager.h>

#include <sstream>
#include <string>

namespace faultline {

namespace {

struct AllocSite {
    clang::SourceLocation loc;
    std::string kind; // "new", "delete", "malloc", "std::make_shared", etc.
    unsigned inLoop = 0;
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

            if (name.find("make_shared") != std::string::npos ||
                name.find("make_unique") != std::string::npos) {
                sites_.push_back({E->getBeginLoc(), name, inLoop_});
            }
        }
        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr *E) {
        if (const auto *CD = E->getConstructor()) {
            std::string parent = CD->getParent()->getQualifiedNameAsString();

            // std::function construction may heap-allocate for large callables.
            if (parent.find("std::function") != std::string::npos) {
                sites_.push_back({E->getBeginLoc(), "std::function ctor", inLoop_});
            }

            // std::shared_ptr construction.
            if (parent.find("std::shared_ptr") != std::string::npos) {
                sites_.push_back({E->getBeginLoc(), "std::shared_ptr ctor", inLoop_});
            }

            // std::string construction (SSO may save us, but non-trivial strings allocate).
            if (parent.find("std::basic_string") != std::string::npos ||
                parent.find("std::__cxx11::basic_string") != std::string::npos) {
                sites_.push_back({E->getBeginLoc(), "std::string ctor", inLoop_});
            }

            // std::vector, std::map, std::unordered_map construction.
            if (parent.find("std::vector") != std::string::npos ||
                parent.find("std::map") != std::string::npos ||
                parent.find("std::unordered_map") != std::string::npos ||
                parent.find("std::list") != std::string::npos ||
                parent.find("std::deque") != std::string::npos) {
                sites_.push_back({E->getBeginLoc(), parent + " ctor", inLoop_});
            }
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

    std::string_view getHardwareMechanism() const override {
        return "Allocator lock contention (glibc malloc arena locks). "
               "TLB pressure from new page mappings. "
               "Page fault jitter. Heap fragmentation degrades spatial locality.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle &Oracle,
                 const Config & /*Cfg*/,
                 std::vector<Diagnostic> &out) override {

        const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return;

        if (!Oracle.isFunctionHot(FD))
            return;

        AllocVisitor visitor(Ctx);
        visitor.TraverseStmt(FD->getBody());

        const auto &SM = Ctx.getSourceManager();

        for (const auto &site : visitor.sites()) {
            Severity sev = Severity::Critical;
            std::vector<std::string> escalations;

            if (site.inLoop) {
                escalations.push_back(
                    "Allocation inside loop: per-iteration allocator pressure, "
                    "compounding TLB and fragmentation cost");
            }

            Diagnostic diag;
            diag.ruleID    = "FL020";
            diag.title     = "Heap Allocation in Hot Path";
            diag.severity  = sev;
            diag.confidence = 0.75;
            diag.evidenceTier = EvidenceTier::Likely;
            diag.functionName = FD->getQualifiedNameAsString();

            if (site.loc.isValid()) {
                diag.location.file   = SM.getFilename(SM.getSpellingLoc(site.loc)).str();
                diag.location.line   = SM.getSpellingLineNumber(site.loc);
                diag.location.column = SM.getSpellingColumnNumber(site.loc);
            }

            std::ostringstream hw;
            hw << "'" << site.kind << "' in hot function '"
               << FD->getQualifiedNameAsString()
               << "'. Each allocation may contend on allocator arena locks, "
               << "trigger mmap/brk syscalls, fault new pages into the TLB, "
               << "and fragment the heap reducing spatial locality.";
            diag.hardwareReasoning = hw.str();

            std::ostringstream ev;
            ev << "alloc_type=" << site.kind
               << "; function=" << FD->getQualifiedNameAsString()
               << "; in_loop=" << (site.inLoop ? "yes" : "no")
               << "; hot_path=true";
            diag.structuralEvidence = ev.str();

            diag.mitigation =
                "Preallocate buffers. Use arena/slab/pool allocators. "
                "Move allocation to cold initialization path. "
                "Reserve std::vector capacity upfront.";

            diag.escalations = std::move(escalations);
            out.push_back(std::move(diag));
        }
    }
};

FAULTLINE_REGISTER_RULE(FL020_HeapAllocHotPath)

} // namespace faultline
