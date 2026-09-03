// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/striped_array.h"
#include "lshaz/analysis/symbols.h"
#include "lshaz/analysis/layout_safety.h"

#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/RecordLayout.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>

#include <llvm/ADT/SmallPtrSet.h>

#include <fnmatch.h>

#include <algorithm>
#include <map>

namespace lshaz {

namespace {

// Deliberately narrow. 
bool nameIsThreadIdent(llvm::StringRef n) {
    static constexpr llvm::StringLiteral kExact[] = {
        "tid", "thread_id", "thread_index", "thread_idx", "thread_num",
        "thd_id", "cur_tid", "running_tid", "core_id", "cpu_id", "shard_id",
        "gettid", "sched_getcpu", "omp_get_thread_num",
    };
    for (auto e : kExact)
        if (n.equals_insensitive(e)) return true;
    return n.ends_with("_tid") || n.ends_with("_thread_id") ||
           n.ends_with("_thread_index");
}

bool isStdAtomicRecord(clang::QualType QT) {
    const clang::CXXRecordDecl *RD = nullptr;
    if (const auto *TST = QT->getAs<clang::TemplateSpecializationType>()) {
        if (auto TD = TST->getTemplateName().getAsTemplateDecl())
            RD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
                TD->getTemplatedDecl());
    }
    if (!RD) RD = QT->getAsCXXRecordDecl();
    if (!RD) return false;
    std::string qn = RD->getQualifiedNameAsString();
    if (qn == "std::atomic" || qn == "std::atomic_ref") return true;
    if (const auto *CTSD =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(RD))
        if (auto *TD = CTSD->getSpecializedTemplate()) {
            std::string tn = TD->getQualifiedNameAsString();
            return tn == "std::atomic" || tn == "std::atomic_ref";
        }
    return false;
}

const clang::ValueDecl *baseDeclOf(const clang::Expr *E) {
    E = E->IgnoreParenImpCasts();
    if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(E))
        return ME->getMemberDecl();
    if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(E))
        return DRE->getDecl();
    return nullptr;
}

const clang::ArraySubscriptExpr *asSubscript(const clang::Expr *E) {
    if (!E) return nullptr;
    E = E->IgnoreParenImpCasts();
    if (const auto *UO = llvm::dyn_cast<clang::UnaryOperator>(E))
        if (UO->getOpcode() == clang::UO_AddrOf)
            E = UO->getSubExpr()->IgnoreParenImpCasts();
    return llvm::dyn_cast<clang::ArraySubscriptExpr>(E);
}

bool isAtomicWriteBuiltin(llvm::StringRef n) {
    return n.starts_with("__atomic_") || n.starts_with("__c11_atomic_") ||
           n.starts_with("__sync_") || n.starts_with("atomic_fetch") ||
           n == "atomic_store" || n == "atomic_store_explicit" ||
           n == "atomic_exchange" || n == "atomic_exchange_explicit" ||
           n.starts_with("atomic_compare_exchange");
}

class UseVisitor : public clang::RecursiveASTVisitor<UseVisitor> {
public:
    clang::ASTContext &ctx;
    StripedArraySummary &out;
    const std::map<std::string, std::string> &aliases;
    const HotPathOracle &oracle;
    const Config &cfg;
    const clang::FunctionDecl *currentFn = nullptr;
    llvm::SmallPtrSet<const clang::ValueDecl *, 8> inductionVars;

    UseVisitor(clang::ASTContext &C, StripedArraySummary &o,
               const std::map<std::string, std::string> &al,
               const HotPathOracle &orc, const Config &c)
        : ctx(C), out(o), aliases(al), oracle(orc), cfg(c) {}

    bool TraverseFunctionDecl(clang::FunctionDecl *FD) {
        if (!FD->doesThisDeclarationHaveABody())
            return RecursiveASTVisitor::TraverseFunctionDecl(FD);
        const auto *prev = currentFn;
        currentFn = FD;
        bool r = RecursiveASTVisitor::TraverseFunctionDecl(FD);
        currentFn = prev;
        return r;
    }
    bool TraverseCXXMethodDecl(clang::CXXMethodDecl *MD) {
        return TraverseFunctionDecl(MD);
    }

    bool TraverseForStmt(clang::ForStmt *S) {
        auto added = pushInduction(S->getInit());
        bool r = RecursiveASTVisitor::TraverseForStmt(S);
        for (const auto *v : added) inductionVars.erase(v);
        return r;
    }

    // write forms; extracting the subscript from the write itself is
    // exact where a parent lookup on every subscript is not.
    bool VisitBinaryOperator(clang::BinaryOperator *B) {
        if (B->isAssignmentOp())
            noteWrite(asSubscript(B->getLHS()));
        return true;
    }
    bool VisitUnaryOperator(clang::UnaryOperator *U) {
        if (U->isIncrementDecrementOp())
            noteWrite(asSubscript(U->getSubExpr()));
        return true;
    }
    bool VisitCallExpr(clang::CallExpr *CE) {
        const auto *FD = CE->getDirectCallee();
        if (!FD || !FD->getIdentifier() || CE->getNumArgs() == 0)
            return true;
        if (isAtomicWriteBuiltin(FD->getName()))
            noteWrite(asSubscript(CE->getArg(0)));
        return true;
    }
    // C11/GNU atomic builtins are AtomicExpr, not CallExpr, the shape
    // every atomicIncr-style macro lowers to.
    bool VisitAtomicExpr(clang::AtomicExpr *E) {
        switch (E->getOp()) {
        case clang::AtomicExpr::AO__c11_atomic_load:
        case clang::AtomicExpr::AO__atomic_load:
        case clang::AtomicExpr::AO__atomic_load_n:
        case clang::AtomicExpr::AO__opencl_atomic_load:
            return true;
        default:
            break;
        }
        noteWrite(asSubscript(E->getPtr()));
        return true;
    }

    bool VisitCXXMemberCallExpr(clang::CXXMemberCallExpr *CE) {
        const auto *MD = CE->getMethodDecl();
        if (!MD) return true;
        llvm::StringRef n = MD->getName();
        if (n == "store" || n == "exchange" || n.starts_with("fetch_") ||
            n.starts_with("compare_exchange"))
            noteWrite(asSubscript(CE->getImplicitObjectArgument()));
        return true;
    }

    // read side: loop-swept access is aggregation, never striping.
    bool VisitArraySubscriptExpr(clang::ArraySubscriptExpr *E) {
        if (!currentFn) return true;
        if (classify(E->getIdx()) != IndexProvenance::LoopInduction)
            return true;
        if (auto *s = siteFor(E))
            s->aggregators.insert(threadRoleNodeName(currentFn, ctx));
        return true;
    }

private:
    std::vector<const clang::ValueDecl *> pushInduction(const clang::Stmt *init) {
        std::vector<const clang::ValueDecl *> added;
        if (const auto *DS = llvm::dyn_cast_or_null<clang::DeclStmt>(init))
            for (const auto *D : DS->decls())
                if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(D))
                    if (inductionVars.insert(VD).second) added.push_back(VD);
        if (const auto *BO = llvm::dyn_cast_or_null<clang::BinaryOperator>(init))
            if (BO->isAssignmentOp())
                if (const auto *D = baseDeclOf(BO->getLHS()))
                    if (inductionVars.insert(D).second) added.push_back(D);
        return added;
    }

    StripedArraySite *siteFor(const clang::ArraySubscriptExpr *E) {
        const clang::ValueDecl *base = baseDeclOf(E->getBase());
        if (!base) return nullptr;
        std::string k = stripedKeyForDecl(base, ctx);
        auto it = out.find(k);
        if (it != out.end()) return &it->second;
        auto a = aliases.find(k);
        if (a == aliases.end()) return nullptr;
        it = out.find(a->second);
        return it == out.end() ? nullptr : &it->second;
    }

    void noteWrite(const clang::ArraySubscriptExpr *E) {
        if (!E || !currentFn) return;
        auto *s = siteFor(E);
        if (!s) return;
        if (classify(E->getIdx()) != IndexProvenance::ThreadIdent) return;
        std::string wn = threadRoleNodeName(currentFn, ctx);
        s->stripedWriters.insert(wn);
        s->writerTier = std::max(s->writerTier, tierOf(wn));
        if (isTLSDerived(E->getIdx())) s->tlsIndexed = true;
        if (isHandedOverIndex(E->getIdx())) s->indexIsHandedOver = true;
        else                               s->indexIsOwnIdentity = true;
    }

    // Named-function tiers outrank oracle hotness: the oracle reaches
    // most functions through a file glob or transitive propagation,
    // which cannot distinguish a per-connection setup routine from the
    // per-command path in the same file. An explicit name is the more
    // specific statement and wins.
    uint8_t tierOf(const std::string &qualified) {
        std::string plain = currentFn->getNameAsString();
        auto hit = [&](const std::vector<std::string> &pats) {
            for (const auto &p : pats)
                if (fnmatch(p.c_str(), plain.c_str(), 0) == 0 ||
                    fnmatch(p.c_str(), qualified.c_str(), 0) == 0)
                    return true;
            return false;
        };
        if (hit(cfg.dispatchPathPatterns))
            return static_cast<uint8_t>(WriteFrequencyTier::Dispatch);
        if (hit(cfg.tickPathPatterns))
            return static_cast<uint8_t>(WriteFrequencyTier::Tick);
        if (oracle.isFunctionHot(currentFn))
            return static_cast<uint8_t>(WriteFrequencyTier::Hot);
        return static_cast<uint8_t>(WriteFrequencyTier::Unknown);
    }

    // arr[c->tid] reads the tid the object carries, so it names whichever
    // thread owns that object rather than the one executing the write. A
    // queue hands each item to one owner at a time, so this shape can be
    // written entirely from one thread however thread-shaped the name is.
    // arr[tid] and arr[sched_getcpu()] name the writer and stay strong.
    bool isHandedOverIndex(const clang::Expr *idx) {
        return llvm::isa<clang::MemberExpr>(idx->IgnoreParenImpCasts());
    }

    bool isTLSDerived(const clang::Expr *idx) {
        idx = idx->IgnoreParenImpCasts();
        if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(idx))
            if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(DRE->getDecl()))
                return VD->getTLSKind() != clang::VarDecl::TLS_None;
        return false;
    }

    IndexProvenance classify(const clang::Expr *idx) {
        idx = idx->IgnoreParenImpCasts();
        clang::Expr::EvalResult r;
        if (idx->EvaluateAsInt(r, ctx)) return IndexProvenance::ConstantIdx;

        if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(idx)) {
            const auto *D = DRE->getDecl();
            if (inductionVars.count(D)) return IndexProvenance::LoopInduction;
            if (const auto *VD = llvm::dyn_cast<clang::VarDecl>(D))
                if (VD->getTLSKind() != clang::VarDecl::TLS_None)
                    return IndexProvenance::ThreadIdent;
            if (nameIsThreadIdent(D->getName()))
                return IndexProvenance::ThreadIdent;
        }
        if (const auto *ME = llvm::dyn_cast<clang::MemberExpr>(idx))
            if (nameIsThreadIdent(ME->getMemberDecl()->getName()))
                return IndexProvenance::ThreadIdent;
        if (const auto *CE = llvm::dyn_cast<clang::CallExpr>(idx))
            if (const auto *F = CE->getDirectCallee())
                if (F->getIdentifier() && nameIsThreadIdent(F->getName()))
                    return IndexProvenance::ThreadIdent;
        return IndexProvenance::Unknown;
    }
};

} // anonymous namespace

std::string stripedKeyForDecl(const clang::ValueDecl *D,
                              clang::ASTContext &Ctx) {
    if (const auto *FD = llvm::dyn_cast<clang::FieldDecl>(D)) {
        const auto *P = FD->getParent();
        if (!P) return {};
        return P->getCanonicalDecl()->getQualifiedNameAsString() +
               "::" + FD->getNameAsString();
    }
    const auto *VD = llvm::dyn_cast<clang::VarDecl>(D);
    if (!VD || !VD->hasGlobalStorage()) return {};
    // internal linkage collides by name across TUs; the defining file
    // disambiguates. external linkage is unique on its own.
    std::string prefix;
    if (VD->getFormalLinkage() == clang::Linkage::Internal) {
        const auto &SM = Ctx.getSourceManager();
        auto loc = SM.getFileLoc(VD->getLocation());
        if (auto fe = SM.getFileEntryRefForID(SM.getFileID(loc)))
            prefix = fe->getName().str();
    }
    return prefix + "::" + VD->getQualifiedNameAsString();
}

void StripedArrayAnalysis::catalogue(const std::vector<clang::Decl *> &decls) {
    auto add = [&](const clang::ValueDecl *D, clang::QualType QT,
                   uint64_t alignBytes, bool fileStatic,
                   const std::string &typeName) {
        if (QT.isConstQualified()) return;
        const auto *CAT = ctx_.getAsConstantArrayType(QT);
        if (!CAT) return;
        const uint64_t n = CAT->getSize().getZExtValue();
        if (n < 2) return;
        clang::QualType elem = CAT->getElementType();
        if (!canComputeTypeSize(elem, ctx_)) return;
        // stride, not data size: indexing steps by the padded layout size.
        const uint64_t es = ctx_.getTypeSizeInChars(elem).getQuantity();
        if (es == 0 || es >= cfg_.cacheLineBytes) return;

        std::string key = stripedKeyForDecl(D, ctx_);
        if (key.empty()) return;

        StripedArraySite s;
        s.key = key;
        s.displayName = D->getNameAsString();
        s.typeName = typeName;
        s.elemSizeBytes = es;
        s.elemCount = n;
        s.declAlignBytes = alignBytes;
        s.elementIsAtomic = elem.getCanonicalType()->isAtomicType() ||
                            isStdAtomicRecord(elem);
        s.elementIsVolatile = elem.isVolatileQualified();
        s.isFileStatic = fileStatic;
        const auto &SM = ctx_.getSourceManager();
        auto ploc = SM.getPresumedLoc(SM.getFileLoc(D->getLocation()));
        if (ploc.isValid()) { s.file = ploc.getFilename(); s.line = ploc.getLine(); }
        summary_.emplace(std::move(key), std::move(s));
    };

    for (auto *D : decls) {
        if (auto *VD = llvm::dyn_cast<clang::VarDecl>(D)) {
            // getDeclAlign is undefined on a dependent type and segfaults on
            // decltype in an uninstantiated template. add() checks the element
            // type, but that is too late: this argument is evaluated first.
            if (VD->hasGlobalStorage() && !VD->isConstexpr() &&
                canComputeTypeSize(VD->getType(), ctx_))
                add(VD, VD->getType(),
                    ctx_.getDeclAlign(VD).getQuantity(),
                    VD->getFormalLinkage() == clang::Linkage::Internal, {});
            continue;
        }
        const auto *RD = llvm::dyn_cast<clang::RecordDecl>(D);
        if (!RD || !RD->isCompleteDefinition()) continue;
        if (!canComputeRecordLayout(RD, ctx_)) continue;
        const std::string tn =
            RD->getCanonicalDecl()->getQualifiedNameAsString();
        const auto &layout = ctx_.getASTRecordLayout(RD);
        unsigned idx = 0;
        for (const auto *F : RD->fields()) {
            const uint64_t offBytes = layout.getFieldOffset(idx++) / 8;
            // field alignment within the record is what places the array
            // base; a line-aligned record with the array at a line
            // multiple is the only in-record head-alignment that counts.
            const uint64_t eff =
                (layout.getAlignment().getQuantity() >= cfg_.cacheLineBytes &&
                 offBytes % cfg_.cacheLineBytes == 0)
                    ? cfg_.cacheLineBytes
                    : ctx_.getTypeAlignInChars(F->getType()).getQuantity();
            add(F, F->getType(), eff, false, tn);
        }
    }
}

void StripedArrayAnalysis::collectAliases(
        const std::vector<clang::Decl *> &decls) {
    for (auto *D : decls) {
        auto *VD = llvm::dyn_cast<clang::VarDecl>(D);
        if (!VD || !VD->hasGlobalStorage() ||
            !VD->getType()->isPointerType())
            continue;
        const clang::Expr *init = VD->getAnyInitializer();
        if (!init) continue;
        init = init->IgnoreParenImpCasts();

        const clang::ValueDecl *target = nullptr;
        uint64_t originElems = 0;
        if (const auto *UO = llvm::dyn_cast<clang::UnaryOperator>(init)) {
            if (UO->getOpcode() != clang::UO_AddrOf) continue;
            const auto *sub = UO->getSubExpr()->IgnoreParenImpCasts();
            const auto *ASE = llvm::dyn_cast<clang::ArraySubscriptExpr>(sub);
            if (!ASE) continue;
            target = baseDeclOf(ASE->getBase());
            clang::Expr::EvalResult r;
            if (ASE->getIdx()->EvaluateAsInt(r, ctx_))
                originElems = r.Val.getInt().getZExtValue();
        } else if (const auto *DRE = llvm::dyn_cast<clang::DeclRefExpr>(init)) {
            target = DRE->getDecl();   // array-to-pointer decay
        }
        if (!target) continue;

        auto it = summary_.find(stripedKeyForDecl(target, ctx_));
        if (it == summary_.end()) continue;
        aliases_[stripedKeyForDecl(VD, ctx_)] = it->first;
        if (originElems > 0 &&
            it->second.declAlignBytes >= cfg_.cacheLineBytes)
            it->second.hasHeadPaddingOffset = true;
    }
}

void StripedArrayAnalysis::collectUses(const clang::TranslationUnitDecl *TU) {
    if (summary_.empty() || !TU) return;
    UseVisitor v(ctx_, summary_, aliases_, oracle_, cfg_);
    v.TraverseDecl(const_cast<clang::TranslationUnitDecl *>(TU));
}

} // namespace lshaz
