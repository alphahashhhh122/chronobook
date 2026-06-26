#pragma once
// MatchingEngine.h
//
// Drives price-time priority matching. Owns the OrderBook; borrows the pool by
// reference. Fills are value types accumulated in a pre-reserved vector so the
// hot path never mallocs. timestamp is a LOGICAL feed sequence number - not
// wall-clock - so replays are deterministic and diffable.
#include "core/OrderBook.h"
#include "core/Order.h"
#include "core/OrderPool.h"
#include <vector>
#include <cstdint>

namespace chronobook {

struct Fill {
    uint64_t buyOrderId{0};
    uint64_t sellOrderId{0};
    uint32_t price{0};      // ticks
    uint32_t qty{0};
    uint64_t timestamp{0};  // logical feed sequence number
};

class MatchingEngine {
public:
    explicit MatchingEngine(OrderPool& pool) : m_pool(pool) {
        m_fills.reserve(1u << 16);   // avoid hot-path realloc
    }

    // Submit an order. Engine takes lifetime responsibility: a resting residual
    // is added to the book; a fully-filled / non-resting residual is freed.
    void processOrder(Order* order, uint64_t sequenceNum = 0);

    // Cancel a resting order by id. true if found.
    bool cancelOrder(uint64_t orderId);

    // Modify price/quantity by losing time priority: unlink the resting order,
    // update it in place, and resubmit it through the normal matching path.
    bool modifyOrder(uint64_t orderId, uint32_t newPrice, uint32_t newQty,
                     uint64_t sequenceNum = 0);

    // Move out fills produced since last drain (keeps memory bounded over a
    // long replay). getFills() peeks without draining (tests/analytics).
    std::vector<Fill> drainFills();
    void drainFillsInto(std::vector<Fill>& out);
    const std::vector<Fill>& getFills() const noexcept { return m_fills; }

    const OrderBook& getBook() const noexcept { return m_book; }

private:
    OrderBook        m_book;
    OrderPool&       m_pool;
    std::vector<Fill> m_fills;
    uint64_t         m_currentSeq{0};

    void matchLimit (Order* order);
    void matchMarket(Order* order);
    void matchIOC   (Order* order);

    // Shared loop. respectPrice=false => market order (crosses any level).
    // Returns with `order` partially/fully filled; residual handling is the
    // caller's job (rest vs free).
    void matchOneSide(Order* order, bool respectPrice);

    void executeTrade(Order* resting, Order* incoming,
                      uint32_t matchPrice, uint32_t matchQty);
};

} // namespace chronobook
