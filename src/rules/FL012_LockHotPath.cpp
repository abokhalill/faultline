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

namespace faultline {

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
        if (className.find("mutex") == std::string::npos &&
            className.find("spinlock") == std::string::npos &&
            className.find("shared_mutex") == std::string::npos)
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

        // RAII lock wrappers.
        if (parent.find("lock_guard") != std::string::npos ||
            parent.find("unique_lock") != std::string::npos ||
            parent.find("shared_lock") != std::string::npos ||
            parent.find("scoped_lock") != std::string::npos) {

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

    std::string_view getHardwareMechanism() const override {
        return "Lock convoy: threads serialize on contended mutex, converting "
               "parallel execution to sequential. Blocking locks trigger "
               "futex syscall → context switch (~1-10us). Cache line "
               "contention on mutex internal state.";
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

        LockVisitor visitor;
        visitor.TraverseStmt(FD->getBody());

        const auto &SM = Ctx.getSourceManager();

        for (const auto &site : visitor.sites()) {
            Severity sev = Severity::Critical;
            std::vector<std::string> escalations;

            if (site.isNested) {
                escalations.push_back(
                    "Nested lock acquisition: deadlock risk and compounding "
                    "serialization latency");
            }

            if (site.inLoop) {
                escalations.push_back(
                    "Lock inside loop: per-iteration lock convoy risk, "
                    "sustained context switch pressure under contention");
            }

            Diagnostic diag;
            diag.ruleID    = "FL012";
            diag.title     = "Lock in Hot Path";
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
               << "'. Under contention, blocking mutex triggers futex "
               << "syscall and context switch (~1-10us). Even uncontended, "
               << "LOCK CMPXCHG on mutex state drains store buffer.";
            diag.hardwareReasoning = hw.str();

            std::ostringstream ev;
            ev << "lock_type=" << site.kind
               << "; function=" << FD->getQualifiedNameAsString()
               << "; nested=" << (site.isNested ? "yes" : "no")
               << "; in_loop=" << (site.inLoop ? "yes" : "no");
            diag.structuralEvidence = ev.str();

            diag.mitigation =
                "Use lock-free data structures. "
                "Adopt single-writer design pattern. "
                "Partition state to eliminate shared mutable access. "
                "Use try_lock with fallback to avoid blocking.";

            diag.escalations = std::move(escalations);
            out.push_back(std::move(diag));
        }
    }
};

FAULTLINE_REGISTER_RULE(FL012_LockHotPath)

} // namespace faultline
