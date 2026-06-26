#include "core/OrderBook.h"
#include <cassert>

namespace chronobook {

bool OrderBook::addOrder(Order* order) {
    if (!order) return false;
    // Duplicate IDs would orphan the old pointer inside its level (still chained,
    // but unreachable via m_orderMap -> uncancellable leak). Catch it loudly.
    assert(m_orderMap.find(order->orderId) == m_orderMap.end() &&
           "duplicate orderId");
    if (m_orderMap.find(order->orderId) != m_orderMap.end()) return false;

    if (order->side == Side::BUY) m_bids[order->price].addOrder(order);
    else                          m_asks[order->price].addOrder(order);
    m_orderMap[order->orderId] = order;
    return true;
}

Order* OrderBook::cancelOrder(uint64_t orderId) {
    auto it = m_orderMap.find(orderId);
    if (it == m_orderMap.end()) return nullptr;
    Order* order = it->second;
    removeOrder(order);          // erases from level + m_orderMap
    return order;                // caller deallocates back to the pool
}

void OrderBook::removeOrder(Order* order) {
    if (!order) return;
    // find(), NOT operator[]: operator[] would INSERT an empty level if the
    // price was already cleaned up, then erase it again - spurious churn.
    if (order->side == Side::BUY) {
        auto it = m_bids.find(order->price);
        if (it != m_bids.end()) {
            it->second.removeOrder(order);
            if (it->second.isEmpty()) m_bids.erase(it);  // erase by iterator: O(1)
        }
    } else {
        auto it = m_asks.find(order->price);
        if (it != m_asks.end()) {
            it->second.removeOrder(order);
            if (it->second.isEmpty()) m_asks.erase(it);
        }
    }
    m_orderMap.erase(order->orderId);
}

bool OrderBook::containsOrder(uint64_t orderId) const noexcept {
    return m_orderMap.find(orderId) != m_orderMap.end();
}

uint32_t OrderBook::getBestBid() const noexcept {
    return m_bids.empty() ? 0 : m_bids.begin()->first;
}
uint32_t OrderBook::getBestAsk() const noexcept {
    return m_asks.empty() ? 0 : m_asks.begin()->first;
}
uint32_t OrderBook::getSpread() const noexcept {
    const uint32_t bid = getBestBid();
    const uint32_t ask = getBestAsk();
    if (bid > 0 && ask > 0 && ask >= bid) return ask - bid;
    return 0;
}
uint64_t OrderBook::getBestBidQty() const noexcept {
    return m_bids.empty() ? 0 : m_bids.begin()->second.getTotalQty();
}
uint64_t OrderBook::getBestAskQty() const noexcept {
    return m_asks.empty() ? 0 : m_asks.begin()->second.getTotalQty();
}

} // namespace chronobook
