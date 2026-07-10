#pragma once
// PriceLevel.h
//
// All orders resting at ONE price, kept in FIFO (time-priority) order via the
// intrusive list embedded in Order. addOrder is O(1) tail-insert; removeOrder
// is O(1) pointer surgery (4 cases). No allocation, ever.
//
// totalQty invariant: it is decremented exactly once per matched unit - either
// by the engine's decreaseTotalQty(matchQty) during a partial fill, or by
// removeOrder() subtracting the order's *current* remainingQty() on cancel /
// full fill. A fully-consumed order's remainingQty() is already 0, so
// removeOrder() subtracts 0 and never double-counts.
#include "core/Order.h"
#include <cstdint>

namespace chronobook {

class PriceLevel {
public:
    void addOrder(Order* order) noexcept {
        order->next = nullptr;
        order->prev = m_tail;
        if (m_tail) m_tail->next = order;
        else        m_head = order;       // first order at this level
        m_tail = order;
        m_totalQty += order->remainingQty();
        ++m_orderCount;
    }

    void removeOrder(Order* order) noexcept {
        // unlink from prev
        if (order->prev) order->prev->next = order->next;
        else             m_head = order->next;       // was head
        // unlink from next
        if (order->next) order->next->prev = order->prev;
        else             m_tail = order->prev;       // was tail

        m_totalQty -= order->remainingQty();
        --m_orderCount;
        order->next = order->prev = nullptr;          // hygiene: no dangling links
    }

    // Used by the match loop for partial resting fills (engine owns this call).
    void decreaseTotalQty(uint32_t qty) noexcept {
        if (qty > m_totalQty) m_totalQty = 0;
        else m_totalQty -= qty;
    }

    Order*   front()         const noexcept { return m_head; }
    bool     isEmpty()       const noexcept { return m_head == nullptr; }
    uint64_t getTotalQty()   const noexcept { return m_totalQty; }
    uint32_t getOrderCount() const noexcept { return m_orderCount; }

private:
    Order*   m_head{nullptr};
    Order*   m_tail{nullptr};
    uint64_t m_totalQty{0};
    uint32_t m_orderCount{0};
};

} // namespace chronobook
