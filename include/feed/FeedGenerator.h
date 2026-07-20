#pragma once
// FeedGenerator.h
//
// Deterministic synthetic feed. Same seed => byte-identical message stream =>
// reproducible replays (the whole point: you can diff two runs / two engines).
// We seed a std::mt19937_64 explicitly and never touch wall-clock or
// std::random_device, both of which would destroy determinism.
//
// It models a realistic mix: mostly resting limit orders, some marketable
// crosses, occasional market/IOC, and cancels of previously-added ids (a cancel
// may target an already-filled id - a real race the engine handles gracefully).
#include "feed/BinaryProtocol.h"
#include <vector>
#include <random>
#include <cstdint>

namespace chronobook {

struct FeedConfig {
    size_t   numMessages   = 1'000'000;
    uint64_t seed          = 0xC0FFEEULL;
    uint32_t midPrice      = 10000;   // ticks
    uint32_t priceBand     = 50;      // +/- around mid
    uint32_t maxQty        = 500;
    double   cancelProb    = 0.20;
    double   marketProb    = 0.05;
    double   iocProb        = 0.05;
    uint64_t symbolPacked  = 0;        // set from a symbol if you like
};

class FeedGenerator {
public:
    explicit FeedGenerator(FeedConfig cfg) : m_cfg(cfg), m_rng(cfg.seed) {}

    std::vector<FeedMessage> generate() {
        std::vector<FeedMessage> msgs;
        msgs.reserve(m_cfg.numMessages);

        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::uniform_int_distribution<uint32_t> qtyDist(1, m_cfg.maxQty);
        std::uniform_int_distribution<int32_t>  bandDist(
            -static_cast<int32_t>(m_cfg.priceBand),
             static_cast<int32_t>(m_cfg.priceBand));

        uint64_t nextId  = 1;
        uint64_t seq     = 0;
        std::vector<uint64_t> live;       // ids we have ADDed (cancel candidates)
        live.reserve(m_cfg.numMessages / 2);

        for (size_t i = 0; i < m_cfg.numMessages; ++i) {
            ++seq;
            const bool doCancel = !live.empty() && unit(m_rng) < m_cfg.cancelProb;

            if (doCancel) {
                std::uniform_int_distribution<size_t> pick(0, live.size() - 1);
                const size_t idx = pick(m_rng);
                msgs.push_back(makeCancel(seq, live[idx], m_cfg.symbolPacked));
                live[idx] = live.back();  // O(1) swap-remove
                live.pop_back();
            } else {
                const Side side = (unit(m_rng) < 0.5) ? Side::BUY : Side::SELL;
                const uint32_t price = static_cast<uint32_t>(
                    static_cast<int32_t>(m_cfg.midPrice) + bandDist(m_rng));
                const uint32_t qty = qtyDist(m_rng);
                const double roll = unit(m_rng);

                OrderType type = OrderType::LIMIT;
                if (roll < m_cfg.marketProb)                       type = OrderType::MARKET;
                else if (roll < m_cfg.marketProb + m_cfg.iocProb)  type = OrderType::IOC;

                const uint64_t id = nextId++;
                msgs.push_back(makeAdd(seq, id, side, type,
                                       type == OrderType::MARKET ? 0u : price,
                                       qty, m_cfg.symbolPacked));
                if (type == OrderType::LIMIT) live.push_back(id);  // only LIMITs can rest
            }
        }
        return msgs;
    }

private:
    FeedConfig    m_cfg;
    std::mt19937_64 m_rng;
};

} // namespace chronobook
