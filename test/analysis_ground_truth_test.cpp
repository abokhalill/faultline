// Analytical ground truth tests for CacheLineMap.
//
// Parses C++ source fragments via Clang tooling, constructs CacheLineMap,
// and asserts exact field offsets, cache line assignments, atomic counts,
// straddle flags, and bucket populations against compiler-verified values.
//
// These tests guarantee that CacheLineMap agrees with ASTRecordLayout.
// Any refactoring that silently changes field offset computation will
// fail here before reaching production.

#include "lshaz/analysis/CacheLineMap.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Tooling/Tooling.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

int failures = 0;
int passed = 0;

void check(bool cond, const char *msg) {
    if (!cond) {
        std::cerr << "  FAIL: " << msg << "\n";
        ++failures;
    } else {
        ++passed;
    }
}

// Parse source, find the named CXXRecordDecl, run callback with ASTContext.
void withRecord(const std::string &source, const std::string &recordName,
                std::function<void(const clang::CXXRecordDecl *,
                                   clang::ASTContext &)> fn) {
    auto AST = clang::tooling::buildASTFromCode(source, "test_input.cpp",
        std::make_shared<clang::PCHContainerOperations>());
    if (!AST) {
        std::cerr << "  FAIL: AST parse failed for record '" << recordName << "'\n";
        ++failures;
        return;
    }

    class Finder : public clang::RecursiveASTVisitor<Finder> {
    public:
        const std::string &target;
        const clang::CXXRecordDecl *found = nullptr;
        explicit Finder(const std::string &t) : target(t) {}
        bool VisitCXXRecordDecl(clang::CXXRecordDecl *RD) {
            if (RD->isCompleteDefinition() && RD->getNameAsString() == target)
                found = RD;
            return true;
        }
    };

    Finder finder(recordName);
    finder.TraverseDecl(AST->getASTContext().getTranslationUnitDecl());
    if (!finder.found) {
        std::cerr << "  FAIL: record '" << recordName << "' not found in AST\n";
        ++failures;
        return;
    }

    fn(finder.found, AST->getASTContext());
}

// Lookup a field by name in the CacheLineMap fields list.
const lshaz::FieldLineEntry *findField(const lshaz::CacheLineMap &map,
                                        const std::string &name) {
    for (const auto &f : map.fields()) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

// ============================================================
// Test 1: Simple POD struct — no padding, no atomics.
// struct Simple { int a; int b; char c; };
// sizeof = 12 (with 0 padding between a,b,c — but trailing pad to 4 → 12)
// ============================================================
void testSimplePOD() {
    std::cerr << "test: simple POD struct layout\n";
    const char *src = R"(
        struct Simple { int a; int b; char c; };
    )";

    withRecord(src, "Simple", [](const clang::CXXRecordDecl *RD,
                                  clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 12, "sizeof Simple == 12");
        check(map.linesSpanned() == 1, "fits in 1 cache line");
        check(map.totalAtomicFields() == 0, "no atomics");
        check(map.totalMutableFields() == 3, "3 mutable fields");
        check(map.straddlingFields().empty(), "no straddling");

        auto *fa = findField(map, "a");
        auto *fb = findField(map, "b");
        auto *fc = findField(map, "c");
        check(fa && fa->offsetBytes == 0, "a at offset 0");
        check(fa && fa->sizeBytes == 4, "a is 4 bytes");
        check(fb && fb->offsetBytes == 4, "b at offset 4");
        check(fc && fc->offsetBytes == 8, "c at offset 8");
        check(fc && fc->sizeBytes == 1, "c is 1 byte");

        check(fa && fa->startLine == 0 && fa->endLine == 0, "a on line 0");
        check(fb && fb->startLine == 0 && fb->endLine == 0, "b on line 0");
        check(fc && fc->startLine == 0 && fc->endLine == 0, "c on line 0");
    });
}

// ============================================================
// Test 2: Struct with padding — natural alignment of double.
// struct Padded { char x; double y; int z; };
// x at 0 (1B), 7B pad, y at 8 (8B), z at 16 (4B), 4B tail pad → 24B
// ============================================================
void testPaddedStruct() {
    std::cerr << "test: padded struct layout\n";
    const char *src = R"(
        struct Padded { char x; double y; int z; };
    )";

    withRecord(src, "Padded", [](const clang::CXXRecordDecl *RD,
                                  clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 24, "sizeof Padded == 24");
        check(map.linesSpanned() == 1, "fits in 1 cache line");

        auto *fx = findField(map, "x");
        auto *fy = findField(map, "y");
        auto *fz = findField(map, "z");
        check(fx && fx->offsetBytes == 0, "x at offset 0");
        check(fy && fy->offsetBytes == 8, "y at offset 8 (after 7B padding)");
        check(fy && fy->sizeBytes == 8, "y is 8 bytes");
        check(fz && fz->offsetBytes == 16, "z at offset 16");
    });
}

// ============================================================
// Test 3: Struct spanning 2 cache lines.
// struct Wide { char data[65]; };
// 65 bytes → spans lines 0 and 1.
// ============================================================
void testCacheLineSpanning() {
    std::cerr << "test: cache line spanning struct\n";
    const char *src = R"(
        struct Wide { char data[65]; };
    )";

    withRecord(src, "Wide", [](const clang::CXXRecordDecl *RD,
                                clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 65, "sizeof Wide == 65");
        check(map.linesSpanned() == 2, "spans 2 cache lines");

        auto *fd = findField(map, "data");
        check(fd && fd->offsetBytes == 0, "data at offset 0");
        check(fd && fd->sizeBytes == 65, "data is 65 bytes");
        check(fd && fd->straddles, "data straddles line boundary");
        check(fd && fd->startLine == 0 && fd->endLine == 1, "data on lines 0-1");
    });
}

// ============================================================
// Test 4: Struct with std::atomic fields — atomic detection.
// ============================================================
void testAtomicDetection() {
    std::cerr << "test: atomic field detection\n";
    const char *src = R"(
        #include <atomic>
        struct AtomicStruct {
            int plain;
            std::atomic<int> counter;
            std::atomic<bool> flag;
            double value;
        };
    )";

    withRecord(src, "AtomicStruct", [](const clang::CXXRecordDecl *RD,
                                        clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.totalAtomicFields() == 2, "2 atomic fields");
        // plain(4B) + pad(0) + counter(4B) + flag(1B) + pad(7B) + value(8B) = 24B
        // Actual layout: plain@0(4), counter@4(4), flag@8(1), pad(7), value@16(8) = 24B
        check(map.recordSizeBytes() == 24, "sizeof AtomicStruct == 24");

        auto *fc = findField(map, "counter");
        auto *ff = findField(map, "flag");
        auto *fp = findField(map, "plain");
        auto *fv = findField(map, "value");

        check(fc && fc->isAtomic, "counter is atomic");
        check(ff && ff->isAtomic, "flag is atomic");
        check(fp && !fp->isAtomic, "plain is not atomic");
        check(fv && !fv->isAtomic, "value is not atomic");

        check(fc && fc->offsetBytes == 4, "counter at offset 4");
        check(ff && ff->offsetBytes == 8, "flag at offset 8");

        auto pairs = map.atomicPairsOnSameLine();
        check(pairs.size() == 1, "1 atomic pair on same line");
    });
}

// ============================================================
// Test 5: Inheritance — base class fields at base offset.
// struct Base { int x; int y; };
// struct Derived : Base { int z; };
// Layout: x@0, y@4, z@8 → 12B
// ============================================================
void testInheritanceLayout() {
    std::cerr << "test: inheritance layout\n";
    const char *src = R"(
        struct Base { int x; int y; };
        struct Derived : Base { int z; };
    )";

    withRecord(src, "Derived", [](const clang::CXXRecordDecl *RD,
                                   clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 12, "sizeof Derived == 12");

        auto *fx = findField(map, "x");
        auto *fy = findField(map, "y");
        auto *fz = findField(map, "z");
        check(fx && fx->offsetBytes == 0, "base field x at offset 0");
        check(fy && fy->offsetBytes == 4, "base field y at offset 4");
        check(fz && fz->offsetBytes == 8, "derived field z at offset 8");
    });
}

// ============================================================
// Test 6: alignas — forced alignment changes offset layout.
// struct Aligned { char a; alignas(64) int b; };
// a@0, b@64 → sizeof at least 128 (64-byte aligned b, then tail pad)
// ============================================================
void testAlignasLayout() {
    std::cerr << "test: alignas layout\n";
    const char *src = R"(
        struct Aligned { char a; alignas(64) int b; };
    )";

    withRecord(src, "Aligned", [](const clang::CXXRecordDecl *RD,
                                   clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 128, "sizeof Aligned == 128");
        check(map.linesSpanned() == 2, "spans 2 cache lines");

        auto *fa = findField(map, "a");
        auto *fb = findField(map, "b");
        check(fa && fa->offsetBytes == 0, "a at offset 0");
        check(fb && fb->offsetBytes == 64, "b at offset 64 (alignas(64))");
        check(fa && fa->startLine == 0, "a on line 0");
        check(fb && fb->startLine == 1, "b on line 1");
    });
}

// ============================================================
// Test 7: Nested struct — sub-fields are recursively collected.
// struct Inner { int a; int b; };
// struct Outer { Inner inner; int c; };
// inner.a@0, inner.b@4, c@8 → 12B
// ============================================================
void testNestedStruct() {
    std::cerr << "test: nested struct recursive field collection\n";
    const char *src = R"(
        struct Inner { int a; int b; };
        struct Outer { Inner inner; int c; };
    )";

    withRecord(src, "Outer", [](const clang::CXXRecordDecl *RD,
                                 clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 12, "sizeof Outer == 12");

        // CacheLineMap should collect both the top-level field "inner"
        // AND the recursive sub-fields "a" and "b".
        auto *finner = findField(map, "inner");
        check(finner && finner->offsetBytes == 0, "inner at offset 0");
        check(finner && finner->sizeBytes == 8, "inner is 8 bytes");

        // Sub-fields from recursion into Inner.
        auto *fa = findField(map, "a");
        auto *fb = findField(map, "b");
        check(fa && fa->offsetBytes == 0, "inner.a at offset 0");
        check(fb && fb->offsetBytes == 4, "inner.b at offset 4");

        auto *fc = findField(map, "c");
        check(fc && fc->offsetBytes == 8, "c at offset 8");
    });
}

// ============================================================
// Test 8: Mixed atomic/non-atomic on same line → false sharing candidate.
// struct MixedLine {
//     std::atomic<int> counter;   // 0-3, line 0, atomic
//     int               plain;    // 4-7, line 0, non-atomic mutable
// };
// ============================================================
void testFalseSharingCandidate() {
    std::cerr << "test: false sharing candidate detection\n";
    const char *src = R"(
        #include <atomic>
        struct MixedLine {
            std::atomic<int> counter;
            int plain;
        };
    )";

    withRecord(src, "MixedLine", [](const clang::CXXRecordDecl *RD,
                                     clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.totalAtomicFields() == 1, "1 atomic field");
        check(map.totalMutableFields() == 2, "2 mutable fields");

        auto candidates = map.falseSharingCandidateLines();
        check(candidates.size() == 1, "1 false sharing candidate line");
        if (!candidates.empty())
            check(candidates[0] == 0, "candidate is line 0");

        const auto &buckets = map.buckets();
        check(buckets.size() == 1, "1 bucket");
        if (!buckets.empty()) {
            check(buckets[0].atomicCount == 1, "bucket 0: 1 atomic");
            check(buckets[0].mutableCount == 2, "bucket 0: 2 mutable");
        }
    });
}

// ============================================================
// Test 9: Field straddling cache line boundary.
// Pack to defeat natural alignment padding so the int lands at offset 62.
// #pragma pack(1):
//     pad[62]@0 + straddler(4)@62 + tail(1)@66 = 67B
//     straddler bytes 62-65 span line 0 (0-63) and line 1 (64-127).
// ============================================================
void testFieldStraddling() {
    std::cerr << "test: field straddling cache line boundary\n";
    const char *src = R"(
        #pragma pack(push, 1)
        struct CrossLine {
            char pad[62];
            int straddler;
            char tail;
        };
        #pragma pack(pop)
    )";

    withRecord(src, "CrossLine", [](const clang::CXXRecordDecl *RD,
                                     clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 67, "sizeof CrossLine == 67");
        check(map.linesSpanned() == 2, "spans 2 lines");

        auto *fs = findField(map, "straddler");
        check(fs && fs->offsetBytes == 62, "straddler at offset 62");
        check(fs && fs->sizeBytes == 4, "straddler is 4 bytes");
        check(fs && fs->straddles, "straddler straddles line boundary");
        check(fs && fs->startLine == 0, "straddler starts on line 0");
        check(fs && fs->endLine == 1, "straddler ends on line 1");

        auto straddlers = map.straddlingFields();
        check(straddlers.size() == 1, "1 straddling field");
    });
}

// ============================================================
// Test 10: Custom cache line size (128B for ARM64 Apple).
// struct Small { char data[100]; };
// With 64B lines: 2 lines. With 128B lines: 1 line.
// ============================================================
void testCustomCacheLineSize() {
    std::cerr << "test: custom cache line size (128B)\n";
    const char *src = R"(
        struct Small { char data[100]; };
    )";

    withRecord(src, "Small", [](const clang::CXXRecordDecl *RD,
                                 clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map64(RD, Ctx, 64);
        lshaz::CacheLineMap map128(RD, Ctx, 128);

        check(map64.linesSpanned() == 2, "64B lines: 2 lines");
        check(map128.linesSpanned() == 1, "128B lines: 1 line");
        check(map64.recordSizeBytes() == map128.recordSizeBytes(),
              "sizeof identical regardless of line size");
    });
}

// ============================================================
// Test 11: Empty base optimization.
// struct Empty {};
// struct WithEmpty : Empty { int x; };
// Layout: EBO applies, sizeof WithEmpty == 4.
// ============================================================
void testEmptyBaseOptimization() {
    std::cerr << "test: empty base optimization\n";
    const char *src = R"(
        struct Empty {};
        struct WithEmpty : Empty { int x; };
    )";

    withRecord(src, "WithEmpty", [](const clang::CXXRecordDecl *RD,
                                     clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 4, "sizeof WithEmpty == 4 (EBO)");

        auto *fx = findField(map, "x");
        check(fx && fx->offsetBytes == 0, "x at offset 0");
    });
}

// ============================================================
// Test 12: Mutable keyword field detection.
// struct WithMutable {
//     mutable int cache;
//     const int immutable;
// };
// ============================================================
void testMutableFieldDetection() {
    std::cerr << "test: mutable keyword field detection\n";
    const char *src = R"(
        struct WithMutable {
            mutable int cache;
            const int immutable;
        };
    )";

    withRecord(src, "WithMutable", [](const clang::CXXRecordDecl *RD,
                                       clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        auto *fc = findField(map, "cache");
        auto *fi = findField(map, "immutable");
        check(fc && fc->isMutable, "mutable int cache is mutable");
        check(fi && !fi->isMutable, "const int immutable is not mutable");
        check(map.totalMutableFields() == 1, "1 mutable field");
    });
}

// ============================================================
// Test 13: Bucket population — verify per-line field grouping.
// struct TwoLine {
//     char line0[64];   // exactly fills line 0
//     int  line1_a;     // line 1, offset 64
//     int  line1_b;     // line 1, offset 68
// };
// ============================================================
void testBucketPopulation() {
    std::cerr << "test: bucket population per cache line\n";
    const char *src = R"(
        struct TwoLine {
            char line0[64];
            int  line1_a;
            int  line1_b;
        };
    )";

    withRecord(src, "TwoLine", [](const clang::CXXRecordDecl *RD,
                                   clang::ASTContext &Ctx) {
        lshaz::CacheLineMap map(RD, Ctx, 64);

        check(map.recordSizeBytes() == 72, "sizeof TwoLine == 72");
        check(map.linesSpanned() == 2, "2 cache lines");

        const auto &buckets = map.buckets();
        check(buckets.size() == 2, "2 buckets");

        // line0 array occupies line 0. But as a single field it contributes
        // 1 entry to bucket 0, plus straddles into bucket 1? No — 64 bytes
        // fits exactly in line 0 (bytes 0-63). endLine = (0+64-1)/64 = 0.
        // So bucket 0 has: line0.
        // Bucket 1 has: line1_a, line1_b.
        if (buckets.size() >= 2) {
            // line0 is a single char[64] field on line 0.
            unsigned b0_count = 0;
            for (const auto *f : buckets[0].fields)
                if (f->name == "line0") ++b0_count;
            check(b0_count == 1, "bucket 0 has line0 field");

            unsigned b1_count = 0;
            for (const auto *f : buckets[1].fields) {
                if (f->name == "line1_a" || f->name == "line1_b")
                    ++b1_count;
            }
            check(b1_count == 2, "bucket 1 has line1_a and line1_b");
        }
    });
}

} // anonymous namespace

int main() {
    testSimplePOD();
    testPaddedStruct();
    testCacheLineSpanning();
    testAtomicDetection();
    testInheritanceLayout();
    testAlignasLayout();
    testNestedStruct();
    testFalseSharingCandidate();
    testFieldStraddling();
    testCustomCacheLineSize();
    testEmptyBaseOptimization();
    testMutableFieldDetection();
    testBucketPopulation();

    std::cerr << "\n" << passed << " passed, " << failures << " failed\n";
    if (failures > 0) {
        std::cerr << "ANALYSIS GROUND TRUTH TESTS FAILED\n";
        return 1;
    }
    std::cerr << "All analysis ground truth tests passed.\n";
    return 0;
}
