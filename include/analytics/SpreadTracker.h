#pragma once
// SpreadTracker: best_ask - best_bid sampled after each message.
// Skips samples where a side is empty (spread undefined). Running mean/min/max.
#include "core/OrderBook.h"
#include "replay/ReplayStats.h"

namespace chronobook {
class SpreadTracker {
public:
    void update(const OrderBook& book) noexcept {
        const uint32_t s = book.getSpread();
        if (book.getBestBid() && book.getBestAsk()) { m_last = s; m_stats.add(s); }
    }
    uint32_t            current() const noexcept { return m_last; }
    const RunningStats& stats()   const noexcept { return m_stats; }
private:
    uint32_t     m_last{0};
    RunningStats m_stats;
};
} // namespace chronobook
