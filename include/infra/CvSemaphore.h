#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace chronobook {

class CvSemaphore {
public:
    explicit CvSemaphore(size_t initial = 0) : m_count(initial) {}

    void release(size_t n = 1) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_count += n;
        }
        for (size_t i = 0; i < n; ++i) m_cv.notify_one();
    }

    void acquire() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] { return m_count > 0 || m_shutdown; });
        if (m_count > 0) --m_count;
    }

    bool tryAcquire() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_count == 0) return false;
        --m_count;
        return true;
    }

    template <typename Rep, typename Period>
    bool tryAcquireFor(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [&] { return m_count > 0 || m_shutdown; })) {
            return false;
        }
        if (m_count == 0) return false;
        --m_count;
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shutdown = true;
        }
        m_cv.notify_all();
    }

    size_t available() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_count;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    size_t m_count{0};
    bool m_shutdown{false};
};

} // namespace chronobook
