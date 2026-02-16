// Phase 4 comprehensive test input.
// Exercises: FL060, FL061, refined EscapeAnalysis (shared_ptr escape)

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

// --- FL060: NUMA-Unfriendly Shared Structure ---
// Large (>256B), thread-escaping (atomics), mutable. Should flag High.
struct MarketDataCache {
    std::atomic<uint64_t> lastSeqNum;
    std::atomic<uint64_t> lastPrice;
    uint64_t bidLevels[32];
    uint64_t askLevels[32];
};

// Large (>4KB), thread-escaping. Should flag Critical.
struct OrderBookSnapshot {
    std::atomic<uint64_t> version;
    uint64_t prices[256];
    uint64_t quantities[256];
};

// Negative: large but no thread escape. Should NOT flag FL060.
struct LocalBuffer {
    char data[1024];
    uint64_t length;
};

// --- FL060 via shared_ptr escape (Phase 4 refinement) ---
// shared_ptr member implies cross-thread sharing. Should flag.
struct SessionState {
    std::shared_ptr<MarketDataCache> cache;
    uint64_t sessionId;
    char metadata[256];
};

// --- FL061: Centralized Dispatcher ---
class IMessageHandler {
public:
    virtual ~IMessageHandler() = default;
    virtual void handleNewOrder(uint64_t id) = 0;
    virtual void handleCancel(uint64_t id) = 0;
    virtual void handleModify(uint64_t id) = 0;
    virtual void handleFill(uint64_t id) = 0;
};

// High fan-out dispatcher: switch + multiple virtual calls. Should flag.
[[clang::annotate("faultline_hot")]]
void dispatchMessage(int msgType, IMessageHandler* handler,
                     uint64_t orderId, std::function<void()> callback) {
    switch (msgType) {
        case 1: handler->handleNewOrder(orderId); break;
        case 2: handler->handleCancel(orderId); break;
        case 3: handler->handleModify(orderId); break;
        case 4: handler->handleFill(orderId); break;
        case 5: callback(); break;
        case 6: handler->handleNewOrder(orderId); handler->handleFill(orderId); break;
        case 7: handler->handleCancel(orderId); handler->handleModify(orderId); break;
        case 8: handler->handleNewOrder(orderId); break;
    }
}

// Dispatch loop — should escalate.
[[clang::annotate("faultline_hot")]]
void processMessageBatch(int* types, uint64_t* ids, int count,
                         IMessageHandler* handler) {
    for (int i = 0; i < count; ++i) {
        switch (types[i]) {
            case 1: handler->handleNewOrder(ids[i]); break;
            case 2: handler->handleCancel(ids[i]); break;
            case 3: handler->handleModify(ids[i]); break;
            case 4: handler->handleFill(ids[i]); break;
        }
    }
}

// Negative: cold path dispatcher. Should NOT flag FL061.
void coldDispatch(int msgType, IMessageHandler* handler) {
    switch (msgType) {
        case 1: handler->handleNewOrder(0); break;
        case 2: handler->handleCancel(0); break;
    }
}
