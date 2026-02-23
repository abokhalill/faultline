// Tier 2 Ground Truth: FL010 — Overly Strong Atomic Ordering
// Paired benchmark: seq_cst store (XCHG on x86-64) vs.
// release store (plain MOV on x86-64 TSO).
//
// Expected: faultline flags seq_cst variant, does NOT flag release variant.
// Expected: perf shows higher cycles/op on seq_cst stores due to XCHG.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>

static constexpr int ITERATIONS = 50'000'000;

[[clang::annotate("faultline_hot")]]
__attribute__((noinline))
void bench_seq_cst(std::atomic<uint64_t>& val) {
    for (int i = 0; i < ITERATIONS; ++i)
        val.store(i, std::memory_order_seq_cst);
}

[[clang::annotate("faultline_hot")]]
__attribute__((noinline))
void bench_release(std::atomic<uint64_t>& val) {
    for (int i = 0; i < ITERATIONS; ++i)
        val.store(i, std::memory_order_release);
}

template <typename Fn>
__attribute__((noinline))
long run(const char* label, Fn fn) {
    std::atomic<uint64_t> val{0};
    auto start = std::chrono::high_resolution_clock::now();
    fn(val);
    auto end = std::chrono::high_resolution_clock::now();
    // Prevent val from being optimized away
    asm volatile("" : : "r"(val.load(std::memory_order_relaxed)) : "memory");
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                label, ns, static_cast<double>(ns) / ITERATIONS);
    return ns;
}

int main() {
    std::printf("FL010 Atomic Ordering Ground Truth Benchmark\n");
    std::printf("=============================================\n");
    std::printf("Iterations: %d\n\n", ITERATIONS);

    // Warmup
    run("warmup", bench_seq_cst);

    long seq_total = 0, rel_total = 0;
    for (int trial = 0; trial < 5; ++trial) {
        std::printf("\n--- Trial %d ---\n", trial + 1);
        seq_total += run("seq_cst store", bench_seq_cst);
        rel_total += run("release store", bench_release);
    }

    double ratio = static_cast<double>(seq_total) / rel_total;
    std::printf("\n=== Result ===\n");
    std::printf("seq_cst / release ratio: %.2fx\n", ratio);
    std::printf("Expected: >1.5x on x86-64 (XCHG vs MOV)\n");

    return 0;
}
