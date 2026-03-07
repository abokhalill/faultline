#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace hft {

// FL002 target: head/tail atomics share a cache line.
// FL041 target: contended producer/consumer queue pattern.
struct alignas(8) OrderQueue {
    std::atomic<uint64_t> head;
    std::atomic<uint64_t> tail;
    uint64_t padding[6];
};

// FL001 target: 192B struct spans 3 cache lines.
struct OrderBookLevel {
    uint64_t priceFixed;
    uint64_t quantity;
    uint64_t sequenceNumber;
    uint32_t flags;
    uint32_t sourceId;
    char venue[16];
    char symbol[32];
    char reserved[104];
};

// FL011 target: atomic seq under multi-writer contention.
struct SequenceTracker {
    std::atomic<uint64_t> publishSeq;
    std::atomic<uint64_t> consumeSeq;
    uint64_t padding[6];
};

// FL060 target: shared mutable structure with unfavorable NUMA placement.
// FL040 target: global mutable state without thread confinement.
struct MarketDataState {
    std::atomic<uint64_t> lastSeqNum;
    std::atomic<uint64_t> lastTimestamp;
    uint64_t totalMessages;
    uint64_t droppedMessages;
    char feedName[64];
};
extern MarketDataState g_mdState;

// FL001 clean: fits in one cache line. Should NOT be flagged.
struct alignas(64) CompactOrder {
    uint64_t orderId;
    uint64_t price;
    uint32_t qty;
    uint32_t side;
};

class OrderBook {
public:
    void addLevel(const OrderBookLevel &level);
    void removeLevel(uint64_t price);
    const OrderBookLevel *bestBid() const;
    const OrderBookLevel *bestAsk() const;

    // FL012 target: mutex in what would be a hot path.
    void updateLevel(uint64_t price, uint64_t newQty);

private:
    std::mutex mutex_;
    std::vector<OrderBookLevel> bids_;
    std::vector<OrderBookLevel> asks_;
};

} // namespace hft
