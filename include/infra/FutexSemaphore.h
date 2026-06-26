#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

#if defined(__linux__)
#include <cerrno>
#include <climits>
#include <cstdint>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#else
#include <condition_variable>
#include <cstdint>
#include <mutex>
#endif

namespace chronobook {

class FutexSemaphore {
public:
    explicit FutexSemaphore(size_t initial = 0)
        : m_count(static_cast<int>(initial)) {}

    void release(size_t n = 1) {
        m_count.fetch_add(static_cast<int>(n), std::memory_order_release);
#if defined(__linux__)
        if (m_waiters.load(std::memory_order_acquire) > 0) {
            futexWake(static_cast<int>(n));
        }
#else
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_epoch;
        }
        for (size_t i = 0; i < n; ++i) m_cv.notify_one();
#endif
    }

    void acquire() {
        while (!tryAcquire()) {
            if (m_shutdown.load(std::memory_order_acquire)) return;
#if defined(__linux__)
            int expected = 0;
            m_waiters.fetch_add(1, std::memory_order_acq_rel);
            if (m_count.load(std::memory_order_acquire) == 0 &&
                !m_shutdown.load(std::memory_order_acquire)) {
                futexWait(expected, nullptr);
            }
            m_waiters.fetch_sub(1, std::memory_order_acq_rel);
#else
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto seen = m_epoch;
            m_cv.wait(lock, [&] {
                return m_shutdown.load(std::memory_order_acquire) ||
                       m_epoch != seen ||
                       m_count.load(std::memory_order_acquire) > 0;
            });
#endif
        }
    }

    bool tryAcquire() {
        int observed = m_count.load(std::memory_order_acquire);
        while (observed > 0) {
            if (m_count.compare_exchange_weak(observed, observed - 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    template <typename Rep, typename Period>
    bool tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!tryAcquire()) {
            if (m_shutdown.load(std::memory_order_acquire)) return false;
#if defined(__linux__)
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
            timespec ts;
            ts.tv_sec = static_cast<time_t>(remaining.count() / 1000000000LL);
            ts.tv_nsec = static_cast<long>(remaining.count() % 1000000000LL);
            int expected = 0;
            m_waiters.fetch_add(1, std::memory_order_acq_rel);
            int rc = 0;
            if (m_count.load(std::memory_order_acquire) == 0 &&
                !m_shutdown.load(std::memory_order_acquire)) {
                rc = futexWait(expected, &ts);
            }
            m_waiters.fetch_sub(1, std::memory_order_acq_rel);
            if (rc == -1 && errno == ETIMEDOUT) return false;
#else
            std::unique_lock<std::mutex> lock(m_mutex);
            const auto seen = m_epoch;
            if (m_cv.wait_until(lock, deadline, [&] {
                    return m_shutdown.load(std::memory_order_acquire) ||
                           m_epoch != seen ||
                           m_count.load(std::memory_order_acquire) > 0;
                })) {
                continue;
            }
            return false;
#endif
        }
        return true;
    }

    void shutdown() {
        m_shutdown.store(true, std::memory_order_release);
#if defined(__linux__)
        futexWake(INT_MAX);
#else
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_epoch;
        }
        m_cv.notify_all();
#endif
    }

    size_t available() const noexcept {
        const int v = m_count.load(std::memory_order_acquire);
        return v > 0 ? static_cast<size_t>(v) : 0;
    }

private:
#if defined(__linux__)
    int futexWait(int expected, const timespec* timeout) noexcept {
        return static_cast<int>(syscall(SYS_futex,
                                        reinterpret_cast<int*>(&m_count),
                                        FUTEX_WAIT_PRIVATE,
                                        expected,
                                        timeout,
                                        nullptr,
                                        0));
    }

    int futexWake(int n) noexcept {
        return static_cast<int>(syscall(SYS_futex,
                                        reinterpret_cast<int*>(&m_count),
                                        FUTEX_WAKE_PRIVATE,
                                        n,
                                        nullptr,
                                        nullptr,
                                        0));
    }
#endif

    std::atomic<int> m_count{0};
    std::atomic<bool> m_shutdown{false};
#if defined(__linux__)
    std::atomic<int> m_waiters{0};
#endif
#if !defined(__linux__)
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    uint64_t m_epoch{0};
#endif
};

} // namespace chronobook
