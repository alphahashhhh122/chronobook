#pragma once

#include "infra/SPSCRingBuffer.h"
#include "journal/FillJournal.h"
#include "store/TradeStore.h"

#include <atomic>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace chronobook {

class DurabilityPipeline {
public:
    DurabilityPipeline(std::string journalPath, std::string storePath,
                       size_t ringCapacity = 4096,
                       size_t journalCapacity = 1u << 20,
                       size_t batchSize = 256)
        : m_ring(ringCapacity),
          m_journalPath(std::move(journalPath)),
          m_storePath(std::move(storePath)),
          m_journalCapacity(journalCapacity),
          m_batchSize(batchSize ? batchSize : 1) {}

    ~DurabilityPipeline() noexcept { stop(); }

    DurabilityPipeline(const DurabilityPipeline&) = delete;
    DurabilityPipeline& operator=(const DurabilityPipeline&) = delete;

    void start() {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) return;
        m_worker = std::thread([this] { run(); });
    }

    void stop() noexcept {
        m_stop.store(true, std::memory_order_release);
        if (m_worker.joinable()) m_worker.join();
        m_running.store(false, std::memory_order_release);
    }

    bool tryPublish(const Fill& fill) noexcept {
        return !m_stop.load(std::memory_order_acquire) && m_ring.tryPush(fill);
    }

    size_t publishBatch(const std::vector<Fill>& fills) noexcept {
        size_t published = 0;
        for (const auto& fill : fills) {
            if (!tryPublish(fill)) break;
            ++published;
        }
        return published;
    }

    uint64_t persisted() const noexcept { return m_persisted.load(std::memory_order_acquire); }
    std::string error() const {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_error;
    }

private:
    void run() noexcept {
        try {
            FillJournal journal(m_journalPath, m_journalCapacity);
            TradeStore store(m_storePath);
            std::vector<Fill> batch;
            batch.reserve(m_batchSize);

            while (!m_stop.load(std::memory_order_acquire) || !m_ring.empty()) {
                Fill fill;
                if (m_ring.tryPop(fill)) {
                    batch.push_back(fill);
                    if (batch.size() >= m_batchSize) flushBatch(journal, store, batch);
                } else {
                    if (!batch.empty()) flushBatch(journal, store, batch);
                    std::this_thread::yield();
                }
            }
            if (!batch.empty()) flushBatch(journal, store, batch);
            journal.flush();
        } catch (const std::exception& e) {
            setError(e.what());
            m_stop.store(true, std::memory_order_release);
        } catch (...) {
            setError("unknown durability pipeline failure");
            m_stop.store(true, std::memory_order_release);
        }
    }

    void flushBatch(FillJournal& journal, TradeStore& store, std::vector<Fill>& batch) {
        journal.appendBatch(batch);
        store.insertBatch(batch);
        m_persisted.fetch_add(static_cast<uint64_t>(batch.size()), std::memory_order_acq_rel);
        batch.clear();
    }

    void setError(std::string error) {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        m_error = std::move(error);
    }

    SPSCRingBuffer<Fill> m_ring;
    std::string m_journalPath;
    std::string m_storePath;
    size_t m_journalCapacity{0};
    size_t m_batchSize{0};
    std::thread m_worker;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    std::atomic<uint64_t> m_persisted{0};
    mutable std::mutex m_errorMutex;
    std::string m_error;
};

} // namespace chronobook
