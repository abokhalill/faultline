// Tier 2 Ground Truth: FL012 — Lock in Hot Path
// Paired benchmark: mutex-protected increment vs. atomic increment (lock-free).
//
// Expected: faultline flags mutex variant, does NOT flag atomic variant.
// Expected: perf shows elevated context switches and futex syscalls on mutex.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

static constexpr int ITERATIONS = 5'000'000;
static constexpr int NUM_THREADS = 4;

// --- HAZARDOUS: mutex in hot loop ---
[[clang::annotate("faultline_hot")]]
void bench_mutex(std::mutex& mtx, uint64_t& value) {
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; ++i) {
                std::lock_guard<std::mutex> guard(mtx);
                value += 1;
            }
        });
    }
    for (auto& t : threads) t.join();
}

// --- FIXED: lock-free atomic ---
[[clang::annotate("faultline_hot")]]
void bench_atomic(std::atomic<uint64_t>& value) {
    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < ITERATIONS; ++i)
                value.fetch_add(1, std::memory_order_relaxed);
        });
    }
    for (auto& t : threads) t.join();
}

int main() {
    std::printf("FL012 Lock in Hot Path Ground Truth Benchmark\n");
    std::printf("==============================================\n");
    std::printf("Iterations: %d per thread, %d threads\n\n", ITERATIONS, NUM_THREADS);

    for (int trial = 0; trial < 5; ++trial) {
        std::printf("--- Trial %d ---\n", trial + 1);

        {
            std::mutex mtx;
            uint64_t value = 0;
            auto start = std::chrono::high_resolution_clock::now();
            bench_mutex(mtx, value);
            auto end = std::chrono::high_resolution_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                        "mutex", ns,
                        static_cast<double>(ns) / (ITERATIONS * NUM_THREADS));
        }

        {
            std::atomic<uint64_t> value{0};
            auto start = std::chrono::high_resolution_clock::now();
            bench_atomic(value);
            auto end = std::chrono::high_resolution_clock::now();
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
            std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                        "atomic", ns,
                        static_cast<double>(ns) / (ITERATIONS * NUM_THREADS));
        }

        std::printf("\n");
    }

    return 0;
}
