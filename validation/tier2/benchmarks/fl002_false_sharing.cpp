// Tier 2 Ground Truth: FL002 — False Sharing
// Paired benchmark: hazardous (two atomics on same cache line) vs.
// fixed (padded to separate cache lines).
//
// Expected: faultline flags Hazardous, does NOT flag Fixed.
// Expected: perf shows elevated L1D invalidations on Hazardous.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

// --- HAZARDOUS: two atomics on same cache line ---
struct alignas(8) HazardousCounters {
    std::atomic<uint64_t> readCount{0};
    std::atomic<uint64_t> writeCount{0};
};

// --- FIXED: padded to separate cache lines ---
struct FixedCounters {
    alignas(64) std::atomic<uint64_t> readCount{0};
    alignas(64) std::atomic<uint64_t> writeCount{0};
};

static constexpr int ITERATIONS = 10'000'000;
static constexpr int NUM_THREADS = 2;

template <typename T>
void run_benchmark(const char* label, T& counters) {
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.emplace_back([&]() {
        for (int i = 0; i < ITERATIONS; ++i)
            counters.readCount.fetch_add(1, std::memory_order_relaxed);
    });
    threads.emplace_back([&]() {
        for (int i = 0; i < ITERATIONS; ++i)
            counters.writeCount.fetch_add(1, std::memory_order_relaxed);
    });

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::printf("%-20s %12ld ns  (%6.2f ns/op)\n",
                label, ns, static_cast<double>(ns) / (ITERATIONS * NUM_THREADS));
}

int main() {
    std::printf("FL002 False Sharing Ground Truth Benchmark\n");
    std::printf("==========================================\n");
    std::printf("Iterations: %d per thread, %d threads\n\n", ITERATIONS, NUM_THREADS);

    // Warmup
    {
        HazardousCounters warmup;
        run_benchmark("warmup", warmup);
    }

    // Actual runs
    for (int trial = 0; trial < 5; ++trial) {
        std::printf("\n--- Trial %d ---\n", trial + 1);
        {
            HazardousCounters h;
            run_benchmark("hazardous", h);
        }
        {
            FixedCounters f;
            run_benchmark("fixed", f);
        }
    }

    return 0;
}
