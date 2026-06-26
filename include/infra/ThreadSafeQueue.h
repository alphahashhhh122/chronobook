#pragma once
// ThreadSafeQueue.h
//
// Blocking MPMC-safe queue built on std::mutex + std::condition_variable.
// This mutex-based queue is kept as a simple baseline for comparison with the SPSC ring.
//
// Shutdown: close() flips a flag and wakes every waiter. pop() returns
// std::nullopt only once the queue is BOTH closed AND drained - so consumers
// process every queued item before exiting (no lost messages, no deadlock).
//
// Why move-only push/pop: a FeedMessage is cheap, but the queue is generic;
// moving avoids copying larger T and lets it hold move-only types.
#include <mutex>
#include <condition_variable>
#include <queue>
#include <optional>
#include <utility>

namespace chronobook {

template <typename T>
class ThreadSafeQueue {
public:
    ThreadSafeQueue() = default;
    ThreadSafeQueue(const ThreadSafeQueue&)            = delete;  // mutex isn't copyable anyway
    ThreadSafeQueue& operator=(const ThreadSafeQueue&) = delete;

    void push(T value) {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_q.push(std::move(value));
        }
        m_cv.notify_one();          // notify OUTSIDE the lock: woken thread won't
                                    // immediately block on a mutex we still hold
    }

    // Block until an item is available or the queue is closed-and-empty.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_mtx);
        // predicate guards against spurious wakeups (loop, not if)
        m_cv.wait(lk, [this] { return !m_q.empty() || m_closed; });
        if (m_q.empty()) return std::nullopt;   // closed and drained -> sentinel
        T value = std::move(m_q.front());
        m_q.pop();
        return value;
    }

    // Non-blocking: returns nullopt if empty right now.
    std::optional<T> tryPop() {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_q.empty()) return std::nullopt;
        T value = std::move(m_q.front());
        m_q.pop();
        return value;
    }

    // Producer is done. Wake all waiters so they can drain + exit.
    void close() {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_closed = true;
        }
        m_cv.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_closed;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_q.size();
    }

private:
    mutable std::mutex      m_mtx;
    std::condition_variable m_cv;
    std::queue<T>           m_q;
    bool                    m_closed{false};
};

} // namespace chronobook
