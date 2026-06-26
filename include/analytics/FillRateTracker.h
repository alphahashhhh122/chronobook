#pragma once
// FillRateTracker: fraction of incoming orders that produced at least one fill
// over a sliding window. High fill rate => aggressive, marketable order flow
// crossing the spread; low => mostly passive resting orders.
//
// Windowed (ring of the last W order outcomes) so the rate reflects *recent*
// flow, not the whole run. Cumulative rate is also exposed.
#include <vector>
#include <cstdint>

namespace chronobook {
class FillRateTracker {
public:
    explicit FillRateTracker(size_t window = 1000)
        : m_ring(window, 0), m_window(window) {}

    // Call once per incoming order. `filled` = did it trade at all?
    void onOrder(bool filled) noexcept {
        ++m_totalOrders;
        if (filled) ++m_totalFilled;
        // sliding window
        m_windowFilled -= m_ring[m_head];
        const uint8_t v = filled ? 1 : 0;
        m_ring[m_head] = v;
        m_windowFilled += v;
        m_head = (m_head + 1) % m_window;
        if (m_count < m_window) ++m_count;
    }
    double windowRate() const noexcept {
        return m_count ? static_cast<double>(m_windowFilled) / static_cast<double>(m_count) : 0.0;
    }
    double cumulativeRate() const noexcept {
        return m_totalOrders ? static_cast<double>(m_totalFilled) / static_cast<double>(m_totalOrders) : 0.0;
    }
    uint64_t totalOrders() const noexcept { return m_totalOrders; }
    uint64_t totalFilled() const noexcept { return m_totalFilled; }
private:
    std::vector<uint8_t> m_ring;
    size_t   m_window;
    size_t   m_head{0};
    size_t   m_count{0};
    uint64_t m_windowFilled{0};
    uint64_t m_totalOrders{0};
    uint64_t m_totalFilled{0};
};
} // namespace chronobook
