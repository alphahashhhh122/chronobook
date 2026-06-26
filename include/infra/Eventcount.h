#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace chronobook {

class Eventcount {
public:
    uint64_t prepareWait() const noexcept {
        return m_epoch.load(std::memory_order_acquire);
    }

    void commitWait(uint64_t observed) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [&] {
            return m_epoch.load(std::memory_order_acquire) != observed;
        });
    }

    void notifyAll() {
        m_epoch.fetch_add(1, std::memory_order_release);
        m_cv.notify_all();
    }

private:
    std::atomic<uint64_t> m_epoch{0};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
};

} // namespace chronobook
