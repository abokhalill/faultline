#include "feed_handler.h"
#include "matching_engine.h"
#include "order_book.h"

int main() {
    hft::OrderBook book;
    hft::FeedHandler feed;
    hft::MatchingEngine engine;
    hft::PriceTimePriority strategy;

    hft::MarketDataMessage msg{};
    msg.sequenceNumber = 1;
    msg.timestamp = 1000;
    msg.price = 10050;
    msg.quantity = 100;
    msg.msgType = 1;
    msg.flags = 0x1;
    feed.onMessage(msg);

    hft::CompactOrder order{};
    order.orderId = 1;
    order.price = 10050;
    order.qty = 50;
    order.side = 0;

    engine.processOrder(order);

    return 0;
}
