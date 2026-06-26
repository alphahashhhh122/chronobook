#pragma once
// ImbalanceTracker:  (bidQty - askQty) / (bidQty + askQty)  in [-1, +1].
// > 0 => bid side heavier; < 0 => ask side heavier.
#include "core/OrderBook.h"
#include "replay/ReplayStats.h"

namespace chronobook {
class ImbalanceTracker {
public:
    void update(const OrderBook& book) noexcept {
        const uint64_t b = book.getBestBidQty();
        const uint64_t a = book.getBestAskQty();
        const uint64_t denom = b + a;
        if (denom == 0) return;                     // no two-sided book -> undefined
        m_last = (static_cast<double>(b) - static_cast<double>(a))
               /  static_cast<double>(denom);
        m_stats.add(m_last);
    }
    double              current() const noexcept { return m_last; }
    const RunningStats& stats()   const noexcept { return m_stats; }
private:
    double       m_last{0.0};
    RunningStats m_stats;
};
} // namespace chronobook
