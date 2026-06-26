#pragma once
// QueueDepthTracker: resting qty at the top of book = bidQty + askQty.
// Thin queues => price moves easily; tracking depth shows you read the book as
// liquidity, not just price levels.
#include "core/OrderBook.h"
#include "replay/ReplayStats.h"

namespace chronobook {
class QueueDepthTracker {
public:
    void update(const OrderBook& book) noexcept {
        m_bidQty = book.getBestBidQty();
        m_askQty = book.getBestAskQty();
        m_depthStats.add(static_cast<double>(m_bidQty + m_askQty));
    }
    uint64_t            bidQty()  const noexcept { return m_bidQty; }
    uint64_t            askQty()  const noexcept { return m_askQty; }
    const RunningStats& stats()   const noexcept { return m_depthStats; }
private:
    uint64_t     m_bidQty{0};
    uint64_t     m_askQty{0};
    RunningStats m_depthStats;
};
} // namespace chronobook
