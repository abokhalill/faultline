// Tier 2 Ground Truth: FL001 — Cache Line Spanning Struct
// Paired benchmark: large struct iterated sequentially vs.
// split hot/cold struct fitting in fewer cache lines.
//
// Expected: faultline flags LargeStruct, does NOT flag SplitStruct.
// Expected: perf shows elevated L1D misses on LargeStruct iteration.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// --- HAZARDOUS: 192B struct, spans 3 cache lines ---
struct LargeStruct {
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    uint32_t flags;
    char metadata[160];
};

// --- FIXED: hot fields only, fits in 1 cache line ---
struct HotFields {
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    uint32_t flags;
};

static constexpr int N = 1'000'000;
static constexpr int ITERATIONS = 20;

template <typename T>
__attribute__((noinline))
uint64_t sum_prices(const std::vector<T>& data) {
    uint64_t total = 0;
    for (const auto& item : data)
        total += item.price;
    return total;
}

template <typename T>
void run_benchmark(const char* label) {
    std::vector<T> data(N);
    for (int i = 0; i < N; ++i) {
        data[i].id = i;
        data[i].price = i * 100;
        data[i].qty = i % 1000;
        data[i].flags = 0;
    }

    // Warmup
    volatile uint64_t sink = sum_prices(data);
    (void)sink;

    auto start = std::chrono::high_resolution_clock::now();
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        sink = sum_prices(data);
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

    std::printf("%-20s %12ld ns  (%5.2f ns/elem, sizeof=%zu)\n",
                label, ns, static_cast<double>(ns) / (N * ITERATIONS), sizeof(T));
}

int main() {
    std::printf("FL001 Cache Line Spanning Ground Truth Benchmark\n");
    std::printf("=================================================\n");
    std::printf("Elements: %d, Iterations: %d\n\n", N, ITERATIONS);

    for (int trial = 0; trial < 5; ++trial) {
        std::printf("--- Trial %d ---\n", trial + 1);
        run_benchmark<LargeStruct>("large (192B)");
        run_benchmark<HotFields>("split (32B)");
        std::printf("\n");
    }

    return 0;
}
