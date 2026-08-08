// regression: 947ceeb — FL091 entity joins across struct/function anchors
#include <atomic>
#include <cstdint>

struct Telemetry {                  // 5: FL001/FL002 anchor
    std::atomic<uint64_t> a{0};
    std::atomic<uint64_t> b{0};
    uint64_t pad[22];
    std::atomic<uint64_t> c{0};
};
Telemetry g_tel;

__attribute__((hot)) void record(uint64_t v) {   // 13: FL011 anchor
    g_tel.a.fetch_add(v, std::memory_order_relaxed);
    g_tel.b.fetch_add(1, std::memory_order_relaxed);
    g_tel.c.store(v, std::memory_order_relaxed);
}
