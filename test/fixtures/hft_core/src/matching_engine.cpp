#include "matching_engine.h"
#include <cstring>

namespace hft {

bool PriceTimePriority::match(const CompactOrder &incoming,
                               const OrderBookLevel &resting,
                               ExecutionReport &report) {
    if (incoming.price >= resting.priceFixed && incoming.side == 0) {
        report.orderId = incoming.orderId;
        report.price = resting.priceFixed;
        report.filledQty = std::min(incoming.qty, static_cast<uint32_t>(resting.quantity));
        report.status = 1;
        return true;
    }
    if (incoming.price <= resting.priceFixed && incoming.side == 1) {
        report.orderId = incoming.orderId;
        report.price = resting.priceFixed;
        report.filledQty = std::min(incoming.qty, static_cast<uint32_t>(resting.quantity));
        report.status = 1;
        return true;
    }
    return false;
}

bool ProRataPriority::match(const CompactOrder &incoming,
                             const OrderBookLevel &resting,
                             ExecutionReport &report) {
    if (resting.quantity == 0) return false;
    report.orderId = incoming.orderId;
    report.price = resting.priceFixed;
    report.filledQty = incoming.qty / 2;
    report.status = 2;
    return report.filledQty > 0;
}

void MatchingEngine::registerCallback(FillCallback cb) {
    callbacks_.push_back(std::move(cb));
}

// FL020: heap allocation in hot processing path.
ExecutionReport *MatchingEngine::allocateReport() {
    return new ExecutionReport{};
}

void MatchingEngine::processOrder(const CompactOrder &order) {
    if (!strategy_) return;

    const OrderBookLevel *best = (order.side == 0)
        ? book_.bestAsk()
        : book_.bestBid();

    if (!best) return;

    // FL020: heap alloc per order in matching loop.
    ExecutionReport *report = allocateReport();

    // FL030: virtual dispatch per order.
    if (strategy_->match(order, *best, *report)) {
        // FL031: std::function invocation per fill.
        for (auto &cb : callbacks_)
            cb(*report);
    }

    delete report;
}

// FL050: deep conditional tree for order validation.
bool MatchingEngine::validateOrder(const CompactOrder &order,
                                    uint32_t sessionFlags,
                                    uint32_t marketPhase,
                                    uint32_t riskTier,
                                    uint32_t accountType) {
    if (order.qty > 0) {
        if (order.price > 0) {
            if (sessionFlags & 0x1) {
                if (marketPhase == 1) {
                    if (riskTier < 3) {
                        if (accountType == 0 || accountType == 1) {
                            return order.qty < 1000000;
                        }
                    }
                } else if (marketPhase == 2) {
                    if (riskTier < 5) {
                        return order.qty < 500000;
                    }
                }
            }
        }
    }
    return false;
}

} // namespace hft
