#include "replay/ReplayEngine.h"
#include <chrono>

namespace chronobook {

size_t ReplayEngine::applyMessage(const FeedMessage& m, ReplayStats& stats) {
    ++stats.messages;
    size_t fillsThisMsg = 0;

    switch (m.msgType) {
        case MsgType::ADD: {
            ++stats.adds;
            Order* o = m_pool.allocate();
            // capacity guard: in a real system you'd backpressure; here we drop.
            if (!o) return 0;
            o->orderId      = m.orderId;
            o->symbolPacked = m.symbolPacked;
            o->price        = m.price;
            o->qty          = m.qty;
            o->filledQty    = 0;
            o->side         = static_cast<Side>(m.side);
            o->type         = static_cast<OrderType>(m.orderType);
            m_engine.processOrder(o, m.sequence);   // engine now owns `o`
            break;
        }
        case MsgType::CANCEL: {
            ++stats.cancels;
            if (!m_engine.cancelOrder(m.orderId)) ++stats.cancelMisses;
            break;
        }
        case MsgType::MODIFY: {
            ++stats.modifies;
            // Price/qty change loses time priority -> modeled as cancel + re-add
            // (standard exchange semantics for a non-trivial modify).
            if (!m_engine.modifyOrder(m.orderId, m.price, m.qty, m.sequence))
                ++stats.cancelMisses;
            break;
        }
    }

    // Pull fills produced by this message (drain keeps memory bounded).
    m_engine.drainFillsInto(m_fillScratch);
    fillsThisMsg = m_fillScratch.size();
    stats.fills += m_fillScratch.size();
    for (const auto& f : m_fillScratch) stats.matchedVolume += f.qty;
    return fillsThisMsg;
}

ReplayStats ReplayEngine::run(const std::vector<FeedMessage>& feed,
                              ReplayMode mode, size_t warmup) {
    ReplayStats stats;

    if (mode == ReplayMode::MAX) {
        // Warm-up: apply but don't time (cold caches, branch predictor warming).
        size_t i = 0;
        for (; i < warmup && i < feed.size(); ++i) applyMessage(feed[i], stats);

        const auto t0 = std::chrono::steady_clock::now();
        for (; i < feed.size(); ++i) applyMessage(feed[i], stats);
        const auto t1 = std::chrono::steady_clock::now();

        stats.elapsedSeconds =
            std::chrono::duration<double>(t1 - t0).count();
    } else {
        const auto t0 = std::chrono::steady_clock::now();
        for (const auto& m : feed) {
            const size_t fills = applyMessage(m, stats);
            // sample analytics off the (const) book after each message
            const OrderBook& book = m_engine.getBook();
            m_spread.update(book);
            m_queueDepth.update(book);
            m_imbalance.update(book);
            if (m.msgType == MsgType::ADD) m_fillRate.onOrder(fills > 0);
        }
        const auto t1 = std::chrono::steady_clock::now();
        stats.elapsedSeconds = std::chrono::duration<double>(t1 - t0).count();
    }
    return stats;
}

} // namespace chronobook
