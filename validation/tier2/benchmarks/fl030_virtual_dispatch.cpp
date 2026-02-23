// Tier 2 Ground Truth: FL030 — Virtual Dispatch in Hot Path
// Paired benchmark: virtual call vs. CRTP static dispatch.
//
// Expected: faultline flags virtual variant, does NOT flag CRTP variant.
// Expected: perf shows elevated branch-misses on virtual dispatch.

#include <chrono>
#include <cstdint>
#include <cstdio>

static constexpr int ITERATIONS = 50'000'000;

// --- HAZARDOUS: virtual dispatch ---
class IHandler {
public:
    virtual ~IHandler() = default;
    virtual uint64_t process(uint64_t val) = 0;
};

class ConcreteHandler : public IHandler {
public:
    uint64_t process(uint64_t val) override { return val + 1; }
};

[[clang::annotate("faultline_hot")]]
__attribute__((noinline))
uint64_t bench_virtual(IHandler* handler) {
    uint64_t sum = 0;
    for (int i = 0; i < ITERATIONS; ++i)
        sum += handler->process(i);
    return sum;
}

// --- FIXED: CRTP static dispatch ---
template <typename Derived>
class CRTPHandler {
public:
    uint64_t process(uint64_t val) {
        return static_cast<Derived*>(this)->processImpl(val);
    }
};

class StaticHandler : public CRTPHandler<StaticHandler> {
public:
    uint64_t processImpl(uint64_t val) { return val + 1; }
};

template <typename T>
[[clang::annotate("faultline_hot")]]
__attribute__((noinline))
uint64_t bench_crtp(T& handler) {
    uint64_t sum = 0;
    for (int i = 0; i < ITERATIONS; ++i)
        sum += handler.process(i);
    return sum;
}

int main() {
    std::printf("FL030 Virtual Dispatch Ground Truth Benchmark\n");
    std::printf("==============================================\n");
    std::printf("Iterations: %d\n\n", ITERATIONS);

    ConcreteHandler vhandler;
    StaticHandler shandler;

    // Warmup
    volatile uint64_t sink = bench_virtual(&vhandler);
    sink = bench_crtp(shandler);
    (void)sink;

    for (int trial = 0; trial < 5; ++trial) {
        std::printf("--- Trial %d ---\n", trial + 1);

        auto start = std::chrono::high_resolution_clock::now();
        sink = bench_virtual(&vhandler);
        auto end = std::chrono::high_resolution_clock::now();
        auto ns_virt = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        start = std::chrono::high_resolution_clock::now();
        sink = bench_crtp(shandler);
        end = std::chrono::high_resolution_clock::now();
        auto ns_crtp = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                    "virtual", ns_virt,
                    static_cast<double>(ns_virt) / ITERATIONS);
        std::printf("%-20s %12ld ns  (%5.2f ns/op)\n",
                    "crtp", ns_crtp,
                    static_cast<double>(ns_crtp) / ITERATIONS);
        std::printf("\n");
    }

    return 0;
}
