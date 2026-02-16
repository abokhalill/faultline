// Phase 1 comprehensive test input.
// Exercises: FL001, FL002, FL020, FL021, FL030, FL031

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

// --- FL001: Cache Line Spanning Struct ---
// 192B, spans 3 cache lines. Should flag High, escalate to Critical (>128B).
struct OrderBook {
    uint64_t bidPrices[12];
    uint64_t askPrices[12];
};

// --- FL002: False Sharing Candidate ---
// Two atomics in a struct < 64B. Cross-thread escape via atomics.
// Should flag Critical.
struct alignas(8) SharedCounters {
    std::atomic<uint64_t> readCount;
    std::atomic<uint64_t> writeCount;
};

// --- FL002 negative: no escape evidence, should NOT flag ---
struct LocalStats {
    uint64_t hits;
    uint64_t misses;
};

// --- FL030/FL031/FL020 need hot-path functions ---

class IHandler {
public:
    virtual ~IHandler() = default;
    virtual void onEvent(uint64_t seqNum) = 0;
};

class ConcreteHandler : public IHandler {
public:
    void onEvent(uint64_t seqNum) override {}
};

// Hot path function: exercises FL020, FL030, FL031
[[clang::annotate("faultline_hot")]]
void processMarketData(IHandler* handler, std::function<void(int)> callback) {
    // FL030: virtual dispatch in hot path
    handler->onEvent(42);

    // FL031: std::function invocation in hot path
    callback(1);

    // FL020: heap allocation in hot path
    auto* p = new uint64_t[16];
    delete[] p;

    // FL020: vector construction (potential heap alloc)
    std::vector<int> tmp;
    tmp.push_back(1);
}

// Hot path with loop escalations
[[clang::annotate("faultline_hot")]]
void processLoop(IHandler* handler, std::function<void(int)> callback) {
    for (int i = 0; i < 1000; ++i) {
        // FL030 escalated: virtual dispatch inside loop
        handler->onEvent(i);

        // FL031 escalated: std::function inside loop
        callback(i);

        // FL020 escalated: allocation inside loop
        auto* buf = new char[256];
        delete[] buf;
    }
}

// --- FL021: Large Stack Frame ---
[[clang::annotate("faultline_hot")]]
void largeStackFunction() {
    char buffer[8192]; // 8KB on stack — exceeds 2KB threshold and page size
    buffer[0] = 'x';
    (void)buffer;
}

// --- Negative: cold path, should NOT trigger FL020/FL030/FL031 ---
void coldPathInit(IHandler* handler) {
    handler->onEvent(0);
    auto* p = new int(42);
    delete p;
}
