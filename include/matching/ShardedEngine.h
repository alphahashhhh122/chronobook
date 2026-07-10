#pragma once
// ShardedEngine.h
//
// Symbol-partitioned matching: the caller routes each incoming message by
// symbol hash into one of N shard threads, each of which owns a private set of
// per-symbol MatchingEngines fed through a lock-free SPSC ring. Shards share no
// mutable book state, keeping each order book owned by exactly one worker.
//
// Within each shard, messages are processed in FIFO order (guaranteed by the
// SPSC ring), preserving per-symbol determinism. Different symbols on the same
// shard each get their own MatchingEngine, so orders never cross-match across
// symbols.
//
// Thread topology:
//
//   Gateway (caller)                    shard 0
//       |--- SPSC[0] ----> worker[0] --> per-symbol MatchingEngines
//       |--- SPSC[1] ----> worker[1] --> per-symbol MatchingEngines
//       |--- SPSC[N] ----> worker[N] --> per-symbol MatchingEngines
//
// Lifetime: start() spawns worker threads; stop() signals shutdown, drains
// remaining messages, and joins. collectAllFills() is valid after stop().
#include "core/OrderPool.h"
#include "feed/BinaryProtocol.h"
#include "infra/CompilerHints.h"
#include "infra/SPSCRingBuffer.h"
#include "matching/MatchingEngine.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace chronobook {

class ShardedEngine {
public:
    // numShards:          number of worker threads (typically = number of cores)
    // poolCapPerShard:    Order slab-pool capacity per shard
    // ringCapacity:       SPSC ring capacity per shard (must be > 0)
    explicit ShardedEngine(size_t numShards,
                           size_t poolCapPerShard = 1u << 16,
                           size_t ringCapacity = 1u << 14)
        : m_numShards(numShards > 0 ? numShards : 1)
    {
        m_shards.reserve(m_numShards);
        for (size_t i = 0; i < m_numShards; ++i) {
            m_shards.push_back(
                std::make_unique<Shard>(poolCapPerShard, ringCapacity));
        }
    }

    ~ShardedEngine() noexcept { stop(); }

    ShardedEngine(const ShardedEngine&) = delete;
    ShardedEngine& operator=(const ShardedEngine&) = delete;

    // Spawn one worker thread per shard. If pinThreads is true, each worker is
    // pinned to core i (Linux: sched_setaffinity; Windows: SetThreadAffinityMask).
    void start(bool pinThreads = false) {
        bool expected = false;
        if (!m_running.compare_exchange_strong(expected, true)) return;
        for (size_t i = 0; i < m_numShards; ++i)
            m_shards[i]->stopFlag.store(false, std::memory_order_release);
        for (size_t i = 0; i < m_numShards; ++i) {
            m_shards[i]->worker = std::thread([this, i, pinThreads] {
                if (pinThreads) pinToCore(static_cast<int>(i));
                workerLoop(*m_shards[i]);
            });
        }
    }

    // Signal all shards to stop, drain remaining messages, and join threads.
    void stop() noexcept {
        if (!m_running.load(std::memory_order_acquire)) return;
        for (auto& s : m_shards)
            s->stopFlag.store(true, std::memory_order_release);
        for (auto& s : m_shards)
            if (s->worker.joinable()) s->worker.join();
        m_running.store(false, std::memory_order_release);
    }

    // Route a single message to the shard that owns its symbol. Returns false
    // if the shard's ring is full (caller should spin/yield and retry).
    bool route(const FeedMessage& msg) noexcept {
        const size_t idx = shardIndex(msg.symbolPacked);
        return m_shards[idx]->inbox.tryPush(msg);
    }

    // Route a batch. Returns the number successfully pushed before backpressure.
    size_t routeBatch(const std::vector<FeedMessage>& msgs) noexcept {
        size_t routed = 0;
        for (const auto& msg : msgs) {
            if (CB_UNLIKELY(!route(msg))) break;
            ++routed;
        }
        return routed;
    }

    // Collect fills from all shards. Call AFTER stop() - not thread-safe while
    // workers are running. Returns fills in timestamp order; stable sort so
    // fills at the same timestamp keep their per-shard production order (e.g.
    // a multi-level sweep produces several fills at the same sequence number).
    std::vector<Fill> collectAllFills() {
        std::vector<Fill> all;
        for (auto& s : m_shards)
            all.insert(all.end(), s->fills.begin(), s->fills.end());
        std::stable_sort(all.begin(), all.end(),
                         [](const Fill& a, const Fill& b) {
                             return a.timestamp < b.timestamp;
                         });
        return all;
    }

    // Total messages processed across all shards (safe to read while running).
    uint64_t totalProcessed() const noexcept {
        uint64_t total = 0;
        for (const auto& s : m_shards)
            total += s->processed.load(std::memory_order_relaxed);
        return total;
    }

    uint64_t shardProcessed(size_t i) const noexcept {
        return m_shards[i]->processed.load(std::memory_order_relaxed);
    }

    size_t numShards() const noexcept { return m_numShards; }

private:
    // Each shard owns a private OrderPool and a map of per-symbol matching
    // engines. The map is only touched by the shard's own worker thread, so no
    // synchronization is needed. The shared pool is fine because the worker is
    // the only allocator/deallocator for orders on this shard.
    struct Shard {
        SPSCRingBuffer<FeedMessage> inbox;
        OrderPool pool;
        std::unordered_map<uint64_t, std::unique_ptr<MatchingEngine>> engines;
        std::vector<Fill> fills;            // accumulated by worker, read after join
        std::vector<Fill> fillScratch;      // reusable drain buffer (avoids alloc)

        alignas(64) std::atomic<uint64_t> processed{0};
        alignas(64) std::atomic<bool> stopFlag{false};
        std::thread worker;

        Shard(size_t poolCap, size_t ringCap)
            : inbox(ringCap), pool(poolCap) {
            fills.reserve(1u << 14);
            fillScratch.reserve(1u << 14);
        }
    };

    // Splittable hash: mix the packed symbol with a multiply-shift so that
    // sequential symbol ids (1, 2, 3, ...) spread evenly across shards.
    size_t shardIndex(uint64_t symbolPacked) const noexcept {
        uint64_t h = symbolPacked;
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        return static_cast<size_t>(h % m_numShards);
    }

    // Look up or create the MatchingEngine for this symbol on this shard.
    MatchingEngine& getEngine(uint64_t symbol, Shard& shard) {
        auto it = shard.engines.find(symbol);
        if (CB_LIKELY(it != shard.engines.end())) return *it->second;
        auto [inserted, ok] = shard.engines.emplace(
            symbol, std::make_unique<MatchingEngine>(shard.pool));
        (void)ok;
        return *inserted->second;
    }

    void workerLoop(Shard& shard) noexcept {
        FeedMessage msg;
        while (!shard.stopFlag.load(std::memory_order_acquire) ||
               !shard.inbox.empty()) {
            if (CB_LIKELY(shard.inbox.tryPop(msg))) {
                applyMessage(msg, shard);
                shard.processed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
        // Final drain: inbox may have received messages between the stop-flag
        // check and the empty() check above.
        while (shard.inbox.tryPop(msg)) {
            applyMessage(msg, shard);
            shard.processed.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void applyMessage(const FeedMessage& msg, Shard& shard) {
        if (CB_UNLIKELY(!isValidFeedMessage(msg))) return;
        MatchingEngine& engine = getEngine(msg.symbolPacked, shard);
        if (msg.msgType == MsgType::ADD) {
            Order* order = shard.pool.allocate();
            if (CB_UNLIKELY(!order)) return;
            order->orderId      = msg.orderId;
            order->symbolPacked = msg.symbolPacked;
            order->price        = msg.price;
            order->qty          = msg.qty;
            order->filledQty    = 0;
            order->side         = static_cast<Side>(msg.side);
            order->type         = static_cast<OrderType>(msg.orderType);
            engine.processOrder(order, msg.sequence);
        } else if (msg.msgType == MsgType::CANCEL) {
            engine.cancelOrder(msg.orderId);
        } else {
            engine.modifyOrder(msg.orderId, msg.price, msg.qty, msg.sequence);
        }
        // Drain fills from this engine into the shard's fill buffer.
        engine.drainFillsInto(shard.fillScratch);
        if (!shard.fillScratch.empty()) {
            shard.fills.insert(shard.fills.end(),
                               shard.fillScratch.begin(),
                               shard.fillScratch.end());
        }
    }

    static void pinToCore(int core) noexcept {
#if defined(__linux__)
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#elif defined(_WIN32)
        SetThreadAffinityMask(GetCurrentThread(),
                              static_cast<DWORD_PTR>(1) << core);
#else
        (void)core;
#endif
    }

    size_t m_numShards;
    std::vector<std::unique_ptr<Shard>> m_shards;
    std::atomic<bool> m_running{false};
};

} // namespace chronobook
