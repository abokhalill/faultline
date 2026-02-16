// Case Study: Simplified HFT Order Matching Engine
// Exercises all 15 Faultline rules in a realistic trading system context.
//
// This is a structural test — not a functional implementation.
// It models common latency landmine patterns found in production HFT systems.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// ============================================================================
// Market Data Structures
// ============================================================================

// FL001: spans 3 cache lines (192B). FL060: NUMA-unfriendly (>256B + atomics).
// FL090: compound hazard (>128B + atomics + thread escape).
struct alignas(8) MarketDataLevel {
    std::atomic<uint64_t> bidPrice;
    std::atomic<uint64_t> askPrice;
    std::atomic<uint64_t> bidQty;
    std::atomic<uint64_t> askQty;
    uint64_t bidOrders[10];
    uint64_t askOrders[10];
    uint64_t timestamps[4];
};

// FL002: false sharing — two atomics on same cache line, cross-thread.
struct alignas(8) SequenceCounters {
    std::atomic<uint64_t> inboundSeq;
    std::atomic<uint64_t> outboundSeq;
};

// FL041: contended queue — head/tail on same cache line.
struct OrderQueue {
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
    uint64_t buffer[4096];
};

// Properly padded — should NOT trigger FL041.
struct PaddedOrderQueue {
    alignas(64) std::atomic<uint64_t> head;
    alignas(64) std::atomic<uint64_t> tail;
    uint64_t buffer[4096];
};

// FL040: centralized mutable global state.
std::atomic<uint64_t> g_globalSequence{0};
std::atomic<bool> g_systemActive{true};

// Negative: const global. Should NOT flag.
const uint64_t MAX_ORDERS = 1000000;

// Negative: thread_local. Should NOT flag.
thread_local uint64_t tl_threadOrderCount = 0;

// ============================================================================
// Order Processing — Hot Path Functions
// ============================================================================

class IOrderHandler {
public:
    virtual ~IOrderHandler() = default;
    virtual void onNewOrder(uint64_t orderId, uint64_t price, uint64_t qty) = 0;
    virtual void onCancel(uint64_t orderId) = 0;
    virtual void onModify(uint64_t orderId, uint64_t newPrice, uint64_t newQty) = 0;
    virtual void onFill(uint64_t orderId, uint64_t fillQty) = 0;
};

// FL030: virtual dispatch in hot path.
// FL031: std::function in hot path.
// FL020: heap allocation in hot path.
// FL010: seq_cst atomic ordering.
// FL012: lock in hot path.
// FL050: deep conditional tree.
// FL061: centralized dispatcher.
[[clang::annotate("faultline_hot")]]
void processOrder(IOrderHandler* handler,
                  std::function<void(uint64_t)> fillCallback,
                  std::mutex& bookMutex,
                  std::atomic<uint64_t>& seqNum,
                  int orderType, uint64_t orderId,
                  uint64_t price, uint64_t qty) {

    // FL010: default seq_cst on load + store.
    uint64_t seq = seqNum.load();
    seqNum.store(seq + 1);

    // FL012: lock in hot path.
    std::lock_guard<std::mutex> guard(bookMutex);

    // FL020: heap allocation in hot path.
    auto* confirmation = new uint64_t[4];
    confirmation[0] = orderId;
    confirmation[1] = price;

    // FL050: deep conditional tree + FL061: centralized dispatch.
    if (orderType == 1) {
        handler->onNewOrder(orderId, price, qty);  // FL030
        if (price > 0) {
            if (qty > 100) {
                if (qty > 1000) {
                    if (price > 50000) {
                        // depth 5: large order fast-path
                        fillCallback(orderId);  // FL031
                    }
                }
            }
        }
    } else if (orderType == 2) {
        handler->onCancel(orderId);  // FL030
    } else if (orderType == 3) {
        handler->onModify(orderId, price, qty);  // FL030
    } else if (orderType == 4) {
        handler->onFill(orderId, qty);  // FL030
        fillCallback(orderId);  // FL031
    }

    delete[] confirmation;  // FL020
}

// FL011: atomic contention hotspot — multiple atomic writes in loop.
[[clang::annotate("faultline_hot")]]
void updateMarketData(MarketDataLevel& level,
                      std::atomic<uint64_t>& globalSeq,
                      const uint64_t* prices, int count) {
    for (int i = 0; i < count; ++i) {
        level.bidPrice.store(prices[i]);
        level.askPrice.store(prices[i] + 1);
        globalSeq.fetch_add(1);
    }
}

// FL021: large stack frame in hot path.
[[clang::annotate("faultline_hot")]]
void buildSnapshot(const MarketDataLevel& level) {
    char snapshotBuffer[16384];  // 16KB on stack
    std::memset(snapshotBuffer, 0, sizeof(snapshotBuffer));
    snapshotBuffer[0] = 'S';
    (void)snapshotBuffer;
}

// Batch processing loop — exercises loop escalations.
[[clang::annotate("faultline_hot")]]
void processBatch(IOrderHandler* handler,
                  std::function<void(uint64_t)> callback,
                  const uint64_t* orderIds, int count) {
    for (int i = 0; i < count; ++i) {
        // FL030 escalated: virtual dispatch in loop.
        handler->onNewOrder(orderIds[i], 0, 0);

        // FL031 escalated: std::function in loop.
        callback(orderIds[i]);

        // FL020 escalated: allocation in loop.
        auto* tmp = new uint64_t(orderIds[i]);
        delete tmp;
    }
}

// ============================================================================
// Cold Path — Should NOT trigger hot-path rules
// ============================================================================

void initializeSystem(IOrderHandler* handler) {
    handler->onNewOrder(0, 0, 0);
    auto* buf = new char[4096];
    delete[] buf;
}

void shutdownSystem(std::atomic<bool>& active) {
    active.store(false, std::memory_order_relaxed);
}
