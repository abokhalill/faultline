// Tier 2 Ground Truth: FL041 — Contended Queue Pattern
// Paired benchmark: head/tail on same cache line vs. padded to separate lines.
//
// Expected: faultline flags UnpaddedQueue, does NOT flag PaddedQueue.
// Expected: perf shows elevated cache invalidations on unpadded variant.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

static constexpr int ITERATIONS = 10'000'000;

// --- HAZARDOUS: head/tail on same cache line ---
struct UnpaddedQueue {
    std::atomic<uint64_t> head{0};
    std::atomic<uint64_t> tail{0};
};

// --- FIXED: head/tail on separate cache lines ---
struct PaddedQueue {
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
};

template <typename Q>
void run_benchmark(const char* label, Q& q) {
    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (int i = 0; i < ITERATIONS; ++i)
            q.tail.store(i, std::memory_order_release);
    });

    std::thread consumer([&]() {
        for (int i = 0; i < ITERATIONS; ++i) {
            while (q.tail.load(std::memory_order_acquire) <= q.head.load(std::memory_order_relaxed))
                ;
            q.head.store(i, std::memory_order_release);
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                label, ns, static_cast<double>(ns) / ITERATIONS);
}

int main() {
    std::printf("FL041 Contended Queue Ground Truth Benchmark\n");
    std::printf("=============================================\n");
    std::printf("Iterations: %d\n\n", ITERATIONS);

    // Warmup
    { UnpaddedQueue q; run_benchmark("warmup", q); }

    for (int trial = 0; trial < 5; ++trial) {
        std::printf("\n--- Trial %d ---\n", trial + 1);
        { UnpaddedQueue q; run_benchmark("unpadded", q); }
        { PaddedQueue q; run_benchmark("padded", q); }
    }

    return 0;
}
