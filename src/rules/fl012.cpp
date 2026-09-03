// SPDX-License-Identifier: Apache-2.0
#include "lshaz/core/rule.h"
#include "lshaz/core/registry.h"
#include "lshaz/core/hot_path.h"

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

namespace lshaz {

namespace {

struct LockSite {
    clang::SourceLocation loc;
    std::string kind;     // "std::mutex::lock", "std::lock_guard", etc.
    bool isNested = false;
    unsigned inLoop = 0;
};

class LockVisitor : public clang::RecursiveASTVisitor<LockVisitor> {
public:
    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *E) {
        const auto *MD = E->getMethodDecl();
        if (!MD)
            return true;

        std::string method = MD->getNameAsString();
        if (method != "lock" && method != "try_lock")
            return true;

        const auto *parent = MD->getParent();
        if (!parent)
            return true;

        std::string className = parent->getQualifiedNameAsString();
        static const char *mutexTypes[] = {
            "std::mutex", "std::recursive_mutex",
            "std::shared_mutex", "std::timed_mutex",
            "std::recursive_timed_mutex", "std::shared_timed_mutex"
        };
        bool isMutex = false;
        for (const auto *mt : mutexTypes) {
            if (className == mt) { isMutex = true; break; }
        }
        // POSIX mutex types.
        if (!isMutex) {
            if (className.find("pthread_mutex_t") != std::string::npos ||
                className.find("pthread_spinlock_t") != std::string::npos ||
                className.find("pthread_rwlock_t") != std::string::npos)
                isMutex = true;
        }
        if (!isMutex)
            return true;

        bool nested = lockDepth_ > 0;
        sites_.push_back({E->getBeginLoc(), className + "::" + method, nested, inLoop_});
        ++lockDepth_;
        ++scopeLockIncrements_;
        return true;
    }

    bool VisitCXXConstructExpr(clang::CXXConstructExpr *E) {
        const auto *CD = E->getConstructor();
        if (!CD)
            return true;

        std::string parent = CD->getParent()->getQualifiedNameAsString();

        // RAII lock wrappers. Resolve through template specializations.
        std::string resolvedName = parent;
        if (const auto *CTSD = llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(
                CD->getParent())) {
            if (auto *TD = CTSD->getSpecializedTemplate())
                resolvedName = TD->getQualifiedNameAsString();
        }
        if (resolvedName == "std::lock_guard" ||
            resolvedName == "std::unique_lock" ||
            resolvedName == "std::shared_lock" ||
            resolvedName == "std::scoped_lock") {

            bool nested = lockDepth_ > 0;
            sites_.push_back({E->getBeginLoc(), parent, nested, inLoop_});
            ++lockDepth_;
            ++scopeLockIncrements_;
        }
        return true;
    }

    bool TraverseCompoundStmt(clang::CompoundStmt *S) {
        unsigned savedDepth = lockDepth_;
        unsigned savedIncrements = scopeLockIncrements_;
        scopeLockIncrements_ = 0;
        bool r = clang::RecursiveASTVisitor<LockVisitor>::TraverseCompoundStmt(S);
        lockDepth_ = savedDepth;
        scopeLockIncrements_ = savedIncrements;
        return r;
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LockVisitor>::TraverseForStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseWhileStmt(clang::WhileStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LockVisitor>::TraverseWhileStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseDoStmt(clang::DoStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LockVisitor>::TraverseDoStmt(S);
        --inLoop_;
        return r;
    }
    bool TraverseCXXForRangeStmt(clang::CXXForRangeStmt *S) {
        ++inLoop_;
        bool r = clang::RecursiveASTVisitor<LockVisitor>::TraverseCXXForRangeStmt(S);
        --inLoop_;
        return r;
    }

    const std::vector<LockSite> &sites() const { return sites_; }

private:
    std::vector<LockSite> sites_;
    unsigned inLoop_ = 0;
    unsigned lockDepth_ = 0;
    unsigned scopeLockIncrements_ = 0;
};

} // anonymous namespace

class FL012_LockHotPath : public Rule {
public:
    std::string_view getID() const override { return "FL012"; }
    std::string_view getTitle() const override { return "Lock in Hot Path"; }
    Severity getBaseSeverity() const override { return Severity::Critical; }

    bool requiresHotPath() const override { return true; }
    bool withdrawnWhenNotHot() const override { return true; }

    std::string_view getHardwareMechanism() const override {
        return "Threads serialise on a contended mutex, converting parallel "
               "execution to sequential: measured 7x the uncontended cost at "
               "two cores. The lock itself is ~14ns uncontended, about 8ns "
               "over an atomic doing the same work. A futex sleep and context "
               "switch costs microseconds, but modern mutexes spin adaptively "
               "and only block when the critical section is long enough to "
               "make spinning wasteful, so that term applies to the length of "
               "the section held, not to the presence of a lock.";
    }

    void analyze(const clang::Decl *D,
                 clang::ASTContext &Ctx,
                 const HotPathOracle &Oracle,
                 const Config & /*Cfg*/,
                 EscapeAnalysis & /*Escape*/,
                 std::vector<Diagnostic> &out) override {

        const auto *FD = llvm::dyn_cast_or_null<clang::FunctionDecl>(D);
        if (!FD || !FD->doesThisDeclarationHaveABody())
            return;

        if (!Oracle.isFunctionHot(FD))
            return;

        LockVisitor visitor;
        visitor.TraverseStmt(FD->getBody());

        const auto &SM = Ctx.getSourceManager();

        for (const auto &site : visitor.sites()) {
            Severity sev = (site.inLoop && site.isNested) ? Severity::Critical
                         : (site.inLoop || site.isNested) ? Severity::High
                                                          : Severity::Medium;
            std::vector<std::string> escalations;

            if (site.isNested) {
                escalations.push_back(
                    "Nested lock acquisition: widens the critical section and "
                    "serializes on two locks, before any contention");
            }

            if (site.inLoop) {
                escalations.push_back(
                    "Lock inside loop: the acquisition cost is paid every "
                    "iteration whether or not the lock is contended");
            }

            escalations.push_back(
                "the convoy cost (futex wait and context switch, ~1-10us) "
                "requires a second thread contending this lock, which is not "
                "established here; severity reflects acquisition frequency "
                "and critical-section width only");

            Diagnostic diag;
            diag.ruleID    = "FL012";
            diag.title     = "Lock in Hot Path";
            diag.severity  = sev;
            diag.confidence = 0.75;
            diag.evidenceTier = EvidenceTier::Likely;
            diag.functionName = FD->getQualifiedNameAsString();

            diag.location = resolveSourceLocation(site.loc, SM);

            std::ostringstream hw;
            hw << "'" << site.kind << "' in hot function '"
               << FD->getQualifiedNameAsString()
               << "'. Under contention, blocking mutex triggers futex "
               << "syscall and context switch (~1-10us). Even uncontended, "
               << "LOCK CMPXCHG on mutex state drains store buffer. "
               << "[Assumes: lock is contended under production load]";
            diag.hardwareReasoning = hw.str();

            diag.structuralEvidence = {
                {"lock_type", site.kind},
                {"function", FD->getQualifiedNameAsString()},
                {"nested", site.isNested ? "yes" : "no"},
                {"in_loop", site.inLoop ? "yes" : "no"},
            };

            diag.mitigation =
                "Use lock-free data structures. "
                "Adopt single-writer design pattern. "
                "Partition state to eliminate shared mutable access. "
                "Use try_lock with fallback to avoid blocking.";

            diag.mechanismClaims = {
                {"lock acquisition cost and critical-section width",
                 "a lock taken on a hot path", true,
                 (site.inLoop && site.isNested) ? Severity::Critical
                 : (site.inLoop || site.isNested) ? Severity::High
                                                  : Severity::Medium},
                {"lock convoy: futex wait and context switch",
                 "a second thread contending this lock", false,
                 Severity::Critical},
            };
            diag.escalations = std::move(escalations);
            out.push_back(std::move(diag));
        }
    }
};

LSHAZ_REGISTER_RULE(FL012_LockHotPath)

} // namespace lshaz
