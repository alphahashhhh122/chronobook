#pragma once
// ReplayEngine.h
//
// Drives a deterministic feed through the MatchingEngine and (in normal mode)
// samples analytics after each message. Two modes:
//
//   NORMAL  - apply each message, drain fills, update all analytics trackers.
//             Use for correctness + microstructure inspection.
//   MAX     - apply messages as fast as possible, no analytics sampling, with a
//             warm-up discard, and measure end-to-end throughput (msgs/sec).
//             Use for the throughput number; per-operation latency is measured separately.
//
// Determinism: messages are applied strictly in feed order by ONE thread, so
// the book/fills are a pure function of the input - two runs are byte-identical
// and diffable against the reference matcher.
//
// Lifetime: ADD allocates an Order from the pool and hands it to the engine,
// which then owns it (rests it or frees it). ReplayEngine never double-frees.
#include "feed/BinaryProtocol.h"
#include "matching/MatchingEngine.h"
#include "replay/ReplayStats.h"
#include "analytics/SpreadTracker.h"
#include "analytics/FillRateTracker.h"
#include "analytics/QueueDepthTracker.h"
#include "analytics/ImbalanceTracker.h"
#include <vector>
#include <cstdint>

namespace chronobook {

enum class ReplayMode { NORMAL, MAX };

class ReplayEngine {
public:
    ReplayEngine(MatchingEngine& engine, OrderPool& pool)
        : m_engine(engine), m_pool(pool) {
        m_fillScratch.reserve(1u << 16);
    }

    // Run a whole feed. In MAX mode, `warmup` leading messages are applied but
    // excluded from the timed window.
    ReplayStats run(const std::vector<FeedMessage>& feed,
                    ReplayMode mode = ReplayMode::NORMAL,
                    size_t warmup = 0);

    // Analytics accessors (meaningful after a NORMAL run).
    const SpreadTracker&     spread()     const noexcept { return m_spread; }
    const FillRateTracker&   fillRate()   const noexcept { return m_fillRate; }
    const QueueDepthTracker& queueDepth() const noexcept { return m_queueDepth; }
    const ImbalanceTracker&  imbalance()  const noexcept { return m_imbalance; }

private:
    // Apply one message. Returns number of fills it produced.
    size_t applyMessage(const FeedMessage& m, ReplayStats& stats);

    MatchingEngine&  m_engine;
    OrderPool&       m_pool;
    SpreadTracker     m_spread;
    FillRateTracker   m_fillRate{1000};
    QueueDepthTracker m_queueDepth;
    ImbalanceTracker  m_imbalance;
    std::vector<Fill> m_fillScratch;
};

} // namespace chronobook
