#include "order_book.h"
#include <algorithm>

namespace hft {

MarketDataState g_mdState;

void OrderBook::addLevel(const OrderBookLevel &level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (level.flags & 0x1)
        bids_.push_back(level);
    else
        asks_.push_back(level);
}

void OrderBook::removeLevel(uint64_t price) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto remove = [price](std::vector<OrderBookLevel> &levels) {
        levels.erase(
            std::remove_if(levels.begin(), levels.end(),
                           [price](const OrderBookLevel &l) {
                               return l.priceFixed == price;
                           }),
            levels.end());
    };
    remove(bids_);
    remove(asks_);
}

const OrderBookLevel *OrderBook::bestBid() const {
    if (bids_.empty()) return nullptr;
    return &*std::max_element(bids_.begin(), bids_.end(),
        [](const OrderBookLevel &a, const OrderBookLevel &b) {
            return a.priceFixed < b.priceFixed;
        });
}

const OrderBookLevel *OrderBook::bestAsk() const {
    if (asks_.empty()) return nullptr;
    return &*std::min_element(asks_.begin(), asks_.end(),
        [](const OrderBookLevel &a, const OrderBookLevel &b) {
            return a.priceFixed < b.priceFixed;
        });
}

void OrderBook::updateLevel(uint64_t price, uint64_t newQty) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto update = [price, newQty](std::vector<OrderBookLevel> &levels) {
        for (auto &l : levels) {
            if (l.priceFixed == price) {
                l.quantity = newQty;
                l.sequenceNumber++;
                return;
            }
        }
    };
    update(bids_);
    update(asks_);
}

} // namespace hft
