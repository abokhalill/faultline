// Tier 2 Ground Truth: FL020 — Heap Allocation in Hot Path
// Paired benchmark: per-iteration new/delete vs. preallocated buffer.
//
// Expected: faultline flags alloc variant, does NOT flag preallocated variant.
// Expected: perf shows elevated dTLB-load-misses and page-faults on alloc.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

static constexpr int ITERATIONS = 2'000'000;

// --- HAZARDOUS: allocation per iteration ---
[[clang::annotate("faultline_hot")]]
__attribute__((noinline))
uint64_t bench_alloc() {
    uint64_t sum = 0;
    for (int i = 0; i < ITERATIONS; ++i) {
        auto* buf = new uint64_t[16];
        buf[0] = i;
        asm volatile("" : : "r"(buf) : "memory");
        sum += buf[0];
        delete[] buf;
    }
    return sum;
}

// --- FIXED: preallocated buffer ---
[[clang::annotate("faultline_hot")]]
__attribute__((noinline))
uint64_t bench_prealloc() {
    uint64_t buf[16];
    uint64_t sum = 0;
    for (int i = 0; i < ITERATIONS; ++i) {
        buf[0] = i;
        asm volatile("" : : "r"(buf) : "memory");
        sum += buf[0];
    }
    return sum;
}

int main() {
    std::printf("FL020 Heap Allocation Ground Truth Benchmark\n");
    std::printf("=============================================\n");
    std::printf("Iterations: %d\n\n", ITERATIONS);

    // Warmup
    volatile uint64_t sink = bench_alloc();
    sink = bench_prealloc();
    (void)sink;

    for (int trial = 0; trial < 5; ++trial) {
        std::printf("--- Trial %d ---\n", trial + 1);

        auto start = std::chrono::high_resolution_clock::now();
        sink = bench_alloc();
        auto end = std::chrono::high_resolution_clock::now();
        auto ns_alloc = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        start = std::chrono::high_resolution_clock::now();
        sink = bench_prealloc();
        end = std::chrono::high_resolution_clock::now();
        auto ns_prealloc = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                    "heap alloc", ns_alloc,
                    static_cast<double>(ns_alloc) / ITERATIONS);
        std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                    "preallocated", ns_prealloc,
                    static_cast<double>(ns_prealloc) / ITERATIONS);
        std::printf("\n");
    }

    return 0;
}
