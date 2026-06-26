#pragma once
// OrderBook.h
//
// Two std::map<price, PriceLevel> (bids descending via std::greater, asks
// ascending) + one unordered_map<id, Order*> for O(1) cancel lookup.
//
// Why map for levels: best bid/ask is begin() in O(1), iteration is in price
// order (needed for matching & analytics), insert/erase of a level is O(log P)
// where P = distinct prices (small). Why unordered_map for ids: cancel is a
// point lookup that wants average O(1); we never iterate ids in order.
//
// Top-of-book quantities are exposed for analytics and replay statistics.
#include "core/Order.h"
#include "core/PriceLevel.h"
#include <map>
#include <unordered_map>
#include <functional>
#include <cstdint>

namespace chronobook {

class OrderBook {
public:
    bool   addOrder(Order* order);
    Order* cancelOrder(uint64_t orderId);   // unlink + return ptr (caller frees)
    void   removeOrder(Order* order);       // used by engine on full fill
    bool   containsOrder(uint64_t orderId) const noexcept;

    uint32_t getBestBid()  const noexcept;  // 0 if empty
    uint32_t getBestAsk()  const noexcept;  // 0 if empty
    uint32_t getSpread()   const noexcept;  // 0 if either side empty / crossed

    // Resting quantity sitting at the top of book.
    uint64_t getBestBidQty() const noexcept;
    uint64_t getBestAskQty() const noexcept;

    // Non-const level access for the matching hot path. nullptr if missing.
    PriceLevel* getBidLevel(uint32_t price) noexcept {
        auto it = m_bids.find(price);
        return it != m_bids.end() ? &it->second : nullptr;
    }
    PriceLevel* getAskLevel(uint32_t price) noexcept {
        auto it = m_asks.find(price);
        return it != m_asks.end() ? &it->second : nullptr;
    }

    const auto& getBids() const noexcept { return m_bids; }
    const auto& getAsks() const noexcept { return m_asks; }

private:
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> m_bids;  // best = begin()
    std::map<uint32_t, PriceLevel>                         m_asks;  // best = begin()
    std::unordered_map<uint64_t, Order*>                   m_orderMap;
};

} // namespace chronobook
