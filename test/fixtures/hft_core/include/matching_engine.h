#pragma once

#include "order_book.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hft {

struct ExecutionReport {
    uint64_t orderId;
    uint64_t execId;
    uint64_t price;
    uint32_t filledQty;
    uint32_t status;
};

// FL030 target: virtual dispatch in hot matching loop.
class MatchingStrategy {
public:
    virtual ~MatchingStrategy() = default;
    virtual bool match(const CompactOrder &incoming,
                       const OrderBookLevel &resting,
                       ExecutionReport &report) = 0;
};

class PriceTimePriority : public MatchingStrategy {
public:
    bool match(const CompactOrder &incoming,
               const OrderBookLevel &resting,
               ExecutionReport &report) override;
};

class ProRataPriority : public MatchingStrategy {
public:
    bool match(const CompactOrder &incoming,
               const OrderBookLevel &resting,
               ExecutionReport &report) override;
};

// FL031 target: std::function in hot path.
// FL061 target: centralized dispatcher routing to many handlers.
class MatchingEngine {
public:
    using FillCallback = std::function<void(const ExecutionReport &)>;

    void registerCallback(FillCallback cb);
    void processOrder(const CompactOrder &order);

    // FL020 target: heap allocation in hot processing path.
    ExecutionReport *allocateReport();

    // FL050 target: deep conditional tree for order validation.
    bool validateOrder(const CompactOrder &order, uint32_t sessionFlags,
                       uint32_t marketPhase, uint32_t riskTier,
                       uint32_t accountType);

private:
    OrderBook book_;
    MatchingStrategy *strategy_ = nullptr;
    std::vector<FillCallback> callbacks_;
};

} // namespace hft
