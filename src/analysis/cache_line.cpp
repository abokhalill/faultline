// SPDX-License-Identifier: Apache-2.0
#include "lshaz/analysis/cache_line.h"

#include "lshaz/analysis/types.h"
#include "lshaz/analysis/layout_safety.h"

#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Type.h>

#include <algorithm>
#include <numeric>

namespace lshaz {

CacheLineMap::CacheLineMap(const clang::RecordDecl *RD,
                           clang::ASTContext &Ctx,
                           uint64_t cacheLineBytes,
                           const std::vector<std::string> &atomicTypeNames)
    : cacheLineBytes_(cacheLineBytes),
      atomicTypeNames_(atomicTypeNames.begin(), atomicTypeNames.end()) {

    if (!canComputeRecordLayout(RD, Ctx))
        return;

    const auto &layout = Ctx.getASTRecordLayout(RD);
    sizeBytes_ = layout.getSize().getQuantity();
    recordAlign_ = layout.getAlignment().getQuantity();
    if (recordAlign_ == 0)
        recordAlign_ = 1;

    // Best case: struct base is cache-line-aligned (offset 0 within line).
    linesSpanned_ = (sizeBytes_ + cacheLineBytes_ - 1) / cacheLineBytes_;

    // Worst case: struct base shifted to maximize cache line span.
    // The base can start at any multiple of recordAlign_ within a cache line.
    // The worst shift is the largest valid shift such that
    // (shift + sizeBytes_) crosses the most line boundaries.
    if (recordAlign_ >= cacheLineBytes_) {
        maxLinesSpanned_ = linesSpanned_;
    } else {
        uint64_t worstShift = cacheLineBytes_ - recordAlign_;
        maxLinesSpanned_ = (worstShift + sizeBytes_ + cacheLineBytes_ - 1) / cacheLineBytes_;
    }

    collectFields(RD, Ctx, 0);
    buildBuckets();
}

bool CacheLineMap::isAtomicType(clang::QualType QT) const {
    QT = peelArrays(QT);
    if (QT.getCanonicalType().isVolatileQualified()) {
        clang::QualType walk = QT;
        while (const auto *TDT = walk->getAs<clang::TypedefType>()) {
            std::string tdName = TDT->getDecl()->getNameAsString();
            std::string lower;
            lower.reserve(tdName.size());
            for (char c : tdName)
                lower.push_back(
                    static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (lower.find("atomic") != std::string::npos)
                return true;
            walk = TDT->desugar();
        }
    }

    // User-configured opaque atomic wrappers (atomic_t, spinlock_t, etc.).
    // must run before canonicalization
    if (!atomicTypeNames_.empty()) {
        if (const auto *RT = QT->getAs<clang::RecordType>()) {
            std::string rn = RT->getDecl()->getNameAsString();
            if (!rn.empty() && atomicTypeNames_.count(rn))
                return true;
        }
        clang::QualType walk = QT;
        while (const auto *TDT = walk->getAs<clang::TypedefType>()) {
            if (atomicTypeNames_.count(TDT->getDecl()->getNameAsString()))
                return true;
            walk = TDT->desugar();
        }
    }

    QT = QT.getCanonicalType();
    if (QT->isAtomicType())
        return true;

    // Canonical record name check (catches typedef'd anonymous structs).
    if (!atomicTypeNames_.empty()) {
        if (const auto *RT = QT->getAs<clang::RecordType>()) {
            std::string rn = RT->getDecl()->getNameAsString();
            if (!rn.empty() && atomicTypeNames_.count(rn))
                return true;
        }
    }

    // C++ std::atomic / std::atomic_ref.
    QT = QT.getNonReferenceType();
    const clang::CXXRecordDecl *RD = nullptr;
    if (const auto *TST = QT->getAs<clang::TemplateSpecializationType>()) {
        if (auto TD = TST->getTemplateName().getAsTemplateDecl())
            RD = llvm::dyn_cast_or_null<clang::CXXRecordDecl>(
                TD->getTemplatedDecl());
    }
    if (!RD)
        RD = QT->getAsCXXRecordDecl();

    if (!RD)
        return false;

    std::string qn = RD->getQualifiedNameAsString();
    if (qn == "std::atomic" || qn == "std::atomic_ref")
        return true;

    if (const auto *CTSD =
            llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(RD)) {
        if (auto *TD = CTSD->getSpecializedTemplate()) {
            std::string tn = TD->getQualifiedNameAsString();
            if (tn == "std::atomic" || tn == "std::atomic_ref")
                return true;
        }
    }

    return false;
}

bool CacheLineMap::hasTrailingLinePad(const clang::RecordDecl *RD,
                                      clang::ASTContext &Ctx,
                                      uint64_t lineBytes) {
    if (!RD || !RD->isCompleteDefinition() || lineBytes == 0)
        return false;
    if (!canComputeRecordLayout(RD, Ctx))
        return false;
    const clang::FieldDecl *last = nullptr;
    for (const auto *f : RD->fields())
        last = f;
    if (!last)
        return false;
    const auto *AT = Ctx.getAsConstantArrayType(last->getType());
    if (!AT || !AT->getElementType()->isCharType())
        return false;
    const auto &layout = Ctx.getASTRecordLayout(RD);
    uint64_t size = layout.getSize().getQuantity();
    if (size == 0 || size % lineBytes != 0)
        return false;
    uint64_t padOffset =
        layout.getFieldOffset(last->getFieldIndex()) / 8;
    uint64_t padSize = Ctx.getTypeSizeInChars(last->getType()).getQuantity();
    return padOffset + padSize == size;
}

bool CacheLineMap::isFieldMutable(const clang::FieldDecl *FD) {
    if (!FD)
        return false;
    if (FD->isMutable())
        return true;
    if (!FD->getType().isConstQualified())
        return true;
    return false;
}

void CacheLineMap::collectFields(const clang::RecordDecl *RD,
                                 clang::ASTContext &Ctx,
                                 uint64_t baseOffsetBytes) {
    if (!canComputeRecordLayout(RD, Ctx))
        return;

    const auto &layout = Ctx.getASTRecordLayout(RD);

    // Base subobjects (C++ only, C structs have no bases).
    if (const auto *CXXRD = llvm::dyn_cast<clang::CXXRecordDecl>(RD)) {
        for (const auto &base : CXXRD->bases()) {
            if (base.isVirtual())
                continue;
            const auto *baseRD = base.getType()->getAsCXXRecordDecl();
            if (!baseRD || !baseRD->isCompleteDefinition())
                continue;
            uint64_t baseOffset = layout.getBaseClassOffset(baseRD).getQuantity();
            collectFields(baseRD, Ctx, baseOffsetBytes + baseOffset);
        }

        // Virtual bases.
        for (const auto &vbase : CXXRD->vbases()) {
            const auto *baseRD = vbase.getType()->getAsCXXRecordDecl();
            if (!baseRD || !baseRD->isCompleteDefinition())
                continue;
            uint64_t baseOffset = layout.getVBaseClassOffset(baseRD).getQuantity();
            collectFields(baseRD, Ctx, baseOffsetBytes + baseOffset);
        }
    }

    // Direct fields.
    unsigned idx = 0;
    for (const auto *field : RD->fields()) {
        // getFieldOffset is positional: idx must advance on skip paths.
        uint64_t offsetBits = layout.getFieldOffset(idx++);
        uint64_t offsetBytes = offsetBits / 8;
        uint64_t absOffset = baseOffsetBytes + offsetBytes;

        if (!canComputeTypeSize(field->getType(), Ctx))
            continue;
        uint64_t fieldSize = Ctx.getTypeSizeInChars(field->getType()).getQuantity();

        // Best case (base at cache line boundary): shift = 0.
        uint64_t startLine = absOffset / cacheLineBytes_;
        uint64_t endByte = absOffset + fieldSize;
        uint64_t endLine = (endByte > 0) ? (endByte - 1) / cacheLineBytes_ : startLine;

        // Worst case: base shifted by maximum valid offset within a cache line.
        // Valid shifts are multiples of recordAlign_ in [0, cacheLineBytes_).
        uint64_t worstShift = (recordAlign_ >= cacheLineBytes_)
            ? 0
            : cacheLineBytes_ - recordAlign_;
        uint64_t wStart = (absOffset + worstShift) / cacheLineBytes_;
        uint64_t wEndByte = absOffset + worstShift + fieldSize;
        uint64_t wEnd = (wEndByte > 0) ? (wEndByte - 1) / cacheLineBytes_ : wStart;

        // A field straddles if ANY valid base alignment causes it to span
        // a cache line boundary. Check all shifts in recordAlign_ steps.
        bool straddles = (startLine != endLine) || (wStart != wEnd);
        if (!straddles && recordAlign_ < cacheLineBytes_) {
            for (uint64_t shift = recordAlign_; shift < cacheLineBytes_;
                 shift += recordAlign_) {
                uint64_t sB = (absOffset + shift) / cacheLineBytes_;
                uint64_t eByte = absOffset + shift + fieldSize;
                uint64_t eL = (eByte > 0) ? (eByte - 1) / cacheLineBytes_ : sB;
                if (sB != eL) { straddles = true; break; }
            }
        }

        // Widest single access the type admits: element size for arrays,
        // scalar size otherwise. Distinguishes geometric spanning (the
        // straddles flag) from split-access risk (straddlingFields()).
        uint64_t granule = fieldSize;
        {
            clang::QualType elemQT = field->getType();
            while (const auto *AT = Ctx.getAsArrayType(elemQT))
                elemQT = AT->getElementType();
            if (canComputeTypeSize(elemQT, Ctx))
                granule = Ctx.getTypeSizeInChars(elemQT).getQuantity();
        }

        // split-access risk is an ELEMENT property. offsets mod line repeat
        // with period line/gcd(granule,line) elements, so the sweep is
        // bounded regardless of extent; never walk a 1MB array.
        bool splits = false;
        if (granule > 1 && granule <= cacheLineBytes_) {
            const uint64_t count = fieldSize / granule;
            const uint64_t period =
                cacheLineBytes_ / std::gcd(granule, cacheLineBytes_);
            const uint64_t kMax = std::max<uint64_t>(1, std::min(count, period));
            const uint64_t shiftStep =
                (recordAlign_ == 0 || recordAlign_ >= cacheLineBytes_)
                    ? cacheLineBytes_
                    : recordAlign_;
            for (uint64_t shift = 0; shift < cacheLineBytes_ && !splits;
                 shift += shiftStep)
                for (uint64_t k = 0; k < kMax; ++k) {
                    uint64_t off =
                        (absOffset + shift + k * granule) % cacheLineBytes_;
                    if (off + granule > cacheLineBytes_) { splits = true; break; }
                }
        }

        bool atomic = isAtomicType(field->getType());
        bool mutable_ = isFieldMutable(field);

        if (atomic) ++totalAtomics_;
        if (mutable_) ++totalMutables_;

        FieldLineEntry entry;
        entry.decl            = field;
        entry.name            = field->getNameAsString();
        entry.offsetBytes     = absOffset;
        entry.sizeBytes       = fieldSize;
        entry.startLine       = startLine;
        entry.endLine         = endLine;
        entry.worstStartLine  = wStart;
        entry.worstEndLine    = wEnd;
        entry.straddles       = straddles;
        entry.accessGranuleBytes = granule;
        entry.splitsAccess    = splits;
        entry.elementCount    = (granule > 0 && fieldSize >= granule)
                                    ? fieldSize / granule : 1;
        entry.isAtomic        = atomic;
        entry.isMutable       = mutable_;

        // Recurse into nested record types for sub-field granularity.
        auto qt = field->getType().getCanonicalType();
        if (const auto *nestedRD = qt->getAsRecordDecl()) {
            if (nestedRD->isCompleteDefinition() && !atomic) {
                collectFields(nestedRD, Ctx, absOffset);
            }
        }

        fields_.push_back(std::move(entry));
    }
}

void CacheLineMap::buildBuckets() {
    if (maxLinesSpanned_ == 0)
        return;

    buckets_.resize(maxLinesSpanned_);
    for (uint64_t i = 0; i < maxLinesSpanned_; ++i)
        buckets_[i].lineIndex = i;

    for (auto &f : fields_) {
        // Union of best-case [startLine, endLine] and worst-case
        // [worstStartLine, worstEndLine]. A field belongs to every bucket
        // it could occupy under any valid struct base alignment.
        uint64_t lo = std::min(f.startLine, f.worstStartLine);
        uint64_t hi = std::max(f.endLine, f.worstEndLine);
        for (uint64_t line = lo; line <= hi && line < maxLinesSpanned_; ++line) {
            buckets_[line].fields.push_back(&f);
            if (f.isAtomic)  ++buckets_[line].atomicCount;
            if (f.isMutable) ++buckets_[line].mutableCount;
        }
    }
}

std::vector<const FieldLineEntry *> CacheLineMap::straddlingFields() const {
    std::vector<const FieldLineEntry *> result;
    for (const auto &f : fields_) {
        if (f.splitsAccess)
            result.push_back(&f);
    }
    return result;
}

namespace {

// a pair whose fields co-occupy several buckets (straddlers, or the
// union of best/worst shift ranges) must count once, not per bucket:
// the duplicate inflated pair-count evidence and escalation lines.
// Two elements of one array on one line. Emitted as a self-pair so every
// consumer of the pair lists inherits array coverage instead of each rule
// growing a private workaround.
void addIntraArrayPairs(const std::vector<CacheLineBucket> &buckets,
                        uint64_t lineBytes, bool atomicOnly,
                        std::vector<CacheLineMap::SharedLinePair> &out) {
    for (const auto &bucket : buckets)
        for (const auto *f : bucket.fields) {
            if (atomicOnly ? !f->isAtomic : !f->isMutable) continue;
            if (!f->elementsShareLine(lineBytes)) continue;
            out.push_back({f, f, bucket.lineIndex, /*intraArray=*/true});
        }
}

void dedupePairs(std::vector<CacheLineMap::SharedLinePair> &pairs) {
    std::sort(pairs.begin(), pairs.end(),
              [](const CacheLineMap::SharedLinePair &x,
                 const CacheLineMap::SharedLinePair &y) {
                  if (x.a != y.a) return x.a < y.a;
                  if (x.b != y.b) return x.b < y.b;
                  return x.lineIndex < y.lineIndex;
              });
    pairs.erase(std::unique(pairs.begin(), pairs.end(),
                            [](const CacheLineMap::SharedLinePair &x,
                               const CacheLineMap::SharedLinePair &y) {
                                return x.a == y.a && x.b == y.b;
                            }),
                pairs.end());
}

} // anonymous namespace

// Two fields co-reside if, under some base alignment the allocator may pick,
// SOME byte of each lands on one line.
bool CacheLineMap::canCoReside(const FieldLineEntry *a,
                               const FieldLineEntry *b) const {
    const uint64_t L = cacheLineBytes_;
    const uint64_t aSize = std::max<uint64_t>(a->sizeBytes, 1);
    const uint64_t bSize = std::max<uint64_t>(b->sizeBytes, 1);

    // step = recordAlign degenerates to the exact same-line test (s=0 only)
    // when the record is line-aligned.
    const uint64_t step = std::min(recordAlign_, L);
    for (uint64_t s = 0; s < L; s += step) {
        const uint64_t aLo = (a->offsetBytes + s) / L;
        const uint64_t aHi = (a->offsetBytes + s + aSize - 1) / L;
        const uint64_t bLo = (b->offsetBytes + s) / L;
        const uint64_t bHi = (b->offsetBytes + s + bSize - 1) / L;
        if (aLo <= bHi && bLo <= aHi)
            return true;
    }
    return false;
}

std::vector<CacheLineMap::SharedLinePair>
CacheLineMap::mutablePairsOnSameLine() const {
    std::vector<SharedLinePair> result;
    for (const auto &bucket : buckets_) {
        for (size_t i = 0; i < bucket.fields.size(); ++i) {
            if (!bucket.fields[i]->isMutable)
                continue;
            for (size_t j = i + 1; j < bucket.fields.size(); ++j) {
                if (!bucket.fields[j]->isMutable)
                    continue;
                if (!canCoReside(bucket.fields[i], bucket.fields[j]))
                    continue;
                result.push_back({bucket.fields[i], bucket.fields[j],
                                  bucket.lineIndex, /*intraArray=*/false});
            }
        }
    }
    addIntraArrayPairs(buckets_, cacheLineBytes_, /*atomicOnly=*/false, result);
    dedupePairs(result);
    return result;
}

std::vector<CacheLineMap::SharedLinePair>
CacheLineMap::atomicPairsOnSameLine() const {
    std::vector<SharedLinePair> result;
    for (const auto &bucket : buckets_) {
        for (size_t i = 0; i < bucket.fields.size(); ++i) {
            if (!bucket.fields[i]->isAtomic)
                continue;
            for (size_t j = i + 1; j < bucket.fields.size(); ++j) {
                if (!bucket.fields[j]->isAtomic)
                    continue;
                if (!canCoReside(bucket.fields[i], bucket.fields[j]))
                    continue;
                result.push_back({bucket.fields[i], bucket.fields[j],
                                  bucket.lineIndex, /*intraArray=*/false});
            }
        }
    }
    addIntraArrayPairs(buckets_, cacheLineBytes_, /*atomicOnly=*/true, result);
    dedupePairs(result);
    return result;
}

std::vector<uint64_t> CacheLineMap::falseSharingCandidateLines() const {
    std::vector<uint64_t> result;
    for (const auto &bucket : buckets_) {
        if (bucket.atomicCount > 0 && bucket.mutableCount > bucket.atomicCount)
            result.push_back(bucket.lineIndex);
    }
    return result;
}

bool CacheLineMap::isRefcountOnly() const {
    if (totalAtomics_ != 1)
        return false;

    // Find the single atomic field and check its name.
    for (const auto &f : fields_) {
        if (!f.isAtomic)
            continue;
        // Normalize to lowercase for comparison.
        std::string lower;
        lower.reserve(f.name.size());
        for (char c : f.name)
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        // Match common refcount field names.  Accept with or without
        // leading underscores / trailing underscores.
        // Strip leading/trailing underscores for matching.
        std::string_view sv(lower);
        while (!sv.empty() && sv.front() == '_') sv.remove_prefix(1);
        while (!sv.empty() && sv.back() == '_') sv.remove_suffix(1);

        if (sv == "ref" || sv == "refs" ||
            sv == "refcount" || sv == "refcnt" ||
            sv == "count" || sv == "cnt" ||
            sv == "nref" || sv == "nrefs" ||
            sv == "rc" || sv == "usecount" ||
            sv == "refcountandflags")
            return true;

        return false;
    }
    return false;
}

} // namespace lshaz
