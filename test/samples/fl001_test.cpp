// FL001 end-to-end validation input.
// Expected: faultline flags LargeOrder as cache-line spanning (192B, 3 lines).
// Expected: faultline flags AtomicHeavy as Critical (>128B + atomics).

#include <atomic>
#include <cstdint>

struct [[clang::annotate("faultline_hot")]] SmallOrder {
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    uint32_t flags;
}; // 24B — should NOT be flagged by FL001.

struct LargeOrder {
    uint64_t id;
    uint64_t price;
    uint32_t qty;
    uint32_t flags;
    char     metadata[160];
}; // 192B — spans 3 cache lines. Should be flagged High.

struct AtomicHeavy {
    std::atomic<uint64_t> seqNum;
    std::atomic<uint64_t> lastPrice;
    char payload[200];
}; // >128B + atomics. Should be flagged Critical.
