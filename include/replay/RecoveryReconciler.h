#pragma once

#include "feed/BinaryProtocol.h"
#include "journal/FillJournal.h"
#include "matching/MatchingEngine.h"
#include "store/TradeStore.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace chronobook {

struct RecoveryReport {
    size_t replayFills{0};
    size_t journalFills{0};
    uint64_t storeTrades{0};
    bool journalMatches{false};
    bool storeCountMatches{false};
    bool storeVwapMatches{false};
    double replayVwap{0.0};
    double storeVwap{0.0};

    bool ok() const noexcept {
        return journalMatches && storeCountMatches && storeVwapMatches;
    }
};

class RecoveryReconciler {
public:
    static std::vector<Fill> replayFills(const std::vector<FeedMessage>& feed,
                                         size_t poolCapacity = 0) {
        if (poolCapacity == 0) poolCapacity = std::max<size_t>(1024, feed.size() + 1024);
        OrderPool pool(poolCapacity);
        MatchingEngine engine(pool);
        std::vector<Fill> fills;
        fills.reserve(feed.size() / 2);

        for (const auto& msg : feed) {
            if (!isValidFeedMessage(msg))
                throw std::runtime_error("invalid feed message during recovery replay");
            if (msg.msgType == MsgType::ADD) {
                Order* order = pool.allocate();
                if (!order) throw std::runtime_error("recovery replay pool exhausted");
                order->orderId = msg.orderId;
                order->symbolPacked = msg.symbolPacked;
                order->price = msg.price;
                order->qty = msg.qty;
                order->filledQty = 0;
                order->side = static_cast<Side>(msg.side);
                order->type = static_cast<OrderType>(msg.orderType);
                engine.processOrder(order, msg.sequence);
            } else if (msg.msgType == MsgType::CANCEL) {
                engine.cancelOrder(msg.orderId);
            } else {
                engine.modifyOrder(msg.orderId, msg.price, msg.qty, msg.sequence);
            }
            auto batch = engine.drainFills();
            fills.insert(fills.end(), batch.begin(), batch.end());
        }
        return fills;
    }

    static RecoveryReport reconcile(const std::vector<FeedMessage>& feed,
                                    const std::string& journalPath,
                                    const std::string& storePath,
                                    size_t poolCapacity = 0) {
        const auto replayed = replayFills(feed, poolCapacity);
        const auto journal = FillJournal::replay(journalPath);
        TradeStore store(storePath);

        RecoveryReport report;
        report.replayFills = replayed.size();
        report.journalFills = journal.size();
        report.storeTrades = store.tradeCount();
        report.journalMatches = sameFills(replayed, journal);
        report.storeCountMatches = report.storeTrades == replayed.size();

        uint64_t beginTs = 0;
        uint64_t endTs = 0;
        report.replayVwap = vwap(replayed, beginTs, endTs);
        report.storeVwap = store.vwap(beginTs, endTs);
        report.storeVwapMatches = std::fabs(report.replayVwap - report.storeVwap) < 1e-9;
        return report;
    }

    static size_t rebuildStoreFromJournal(const std::string& journalPath,
                                          const std::string& storePath) {
        const auto records = FillJournal::replay(journalPath);
        std::vector<Fill> fills;
        fills.reserve(records.size());
        for (const auto& record : records) {
            fills.push_back(Fill{record.buyOrderId, record.sellOrderId,
                                 record.price, record.qty, record.timestamp,
                                 record.symbolPacked});
        }
        TradeStore store(storePath);
        store.replaceAll(fills);
        return fills.size();
    }

private:
    static bool sameFills(const std::vector<Fill>& fills,
                          const std::vector<FillRecord>& records) {
        if (fills.size() != records.size()) return false;
        for (size_t i = 0; i < fills.size(); ++i) {
            if (fills[i].buyOrderId != records[i].buyOrderId ||
                fills[i].sellOrderId != records[i].sellOrderId ||
                fills[i].price != records[i].price ||
                fills[i].qty != records[i].qty ||
                fills[i].timestamp != records[i].timestamp ||
                fills[i].symbolPacked != records[i].symbolPacked) {
                return false;
            }
        }
        return true;
    }

    static double vwap(const std::vector<Fill>& fills, uint64_t& beginTs, uint64_t& endTs) {
        if (fills.empty()) return 0.0;
        beginTs = std::numeric_limits<uint64_t>::max();
        endTs = 0;
        long double notional = 0.0;
        uint64_t qty = 0;
        for (const auto& fill : fills) {
            beginTs = std::min(beginTs, fill.timestamp);
            endTs = std::max(endTs, fill.timestamp);
            notional += static_cast<long double>(fill.price) * fill.qty;
            qty += fill.qty;
        }
        return qty == 0 ? 0.0 : static_cast<double>(notional / qty);
    }
};

} // namespace chronobook
