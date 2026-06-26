#pragma once

#include <atomic>
#include <cstddef>
#include <optional>
#include <vector>

namespace chronobook {

template <typename T>
class SPSCRingBuffer {
public:
    explicit SPSCRingBuffer(size_t capacity)
        : m_slots(capacity + 1), m_capacity(capacity + 1) {}

    SPSCRingBuffer(const SPSCRingBuffer&) = delete;
    SPSCRingBuffer& operator=(const SPSCRingBuffer&) = delete;

    bool tryPush(const T& value) noexcept {
        const size_t head = m_head.value.load(std::memory_order_relaxed);
        const size_t next = increment(head);
        if (next == m_tail.value.load(std::memory_order_acquire)) return false;
        m_slots[head] = value;
        m_head.value.store(next, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) noexcept {
        const size_t tail = m_tail.value.load(std::memory_order_relaxed);
        if (tail == m_head.value.load(std::memory_order_acquire)) return false;
        out = m_slots[tail];
        m_tail.value.store(increment(tail), std::memory_order_release);
        return true;
    }

    std::optional<T> tryPop() noexcept {
        T out{};
        if (!tryPop(out)) return std::nullopt;
        return out;
    }

    bool empty() const noexcept {
        return m_tail.value.load(std::memory_order_acquire) ==
               m_head.value.load(std::memory_order_acquire);
    }

    size_t capacity() const noexcept { return m_capacity - 1; }

private:
    struct alignas(64) PaddedIndex {
        std::atomic<size_t> value{0};
    };

    size_t increment(size_t i) const noexcept { return (i + 1) % m_capacity; }

    std::vector<T> m_slots;
    size_t m_capacity{0};
    PaddedIndex m_head;
    PaddedIndex m_tail;
};

} // namespace chronobook
