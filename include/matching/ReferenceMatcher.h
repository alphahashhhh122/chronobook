#pragma once

#include "feed/BinaryProtocol.h"
#include "matching/MatchingEngine.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace chronobook {

class ReferenceMatcher {
public:
    std::vector<Fill> run(const std::vector<FeedMessage>& feed) {
        m_book.clear();
        m_fills.clear();
        for (const auto& msg : feed) {
            if (isValidFeedMessage(msg)) apply(msg);
        }
        return m_fills;
    }

private:
    struct RestingOrder {
        uint64_t orderId{0};
        uint64_t symbolPacked{0};
        uint32_t price{0};
        uint32_t qty{0};
        uint32_t filledQty{0};
        Side side{Side::BUY};
        uint64_t sequence{0};

        uint32_t remainingQty() const noexcept {
            return filledQty >= qty ? 0u : qty - filledQty;
        }
    };

    void apply(const FeedMessage& msg) {
        switch (msg.msgType) {
            case MsgType::ADD:
                add(msg);
                break;
            case MsgType::CANCEL:
                eraseOrder(msg.orderId);
                break;
            case MsgType::MODIFY: {
                RestingOrder old;
                if (eraseOrder(msg.orderId, &old)) {
                    FeedMessage replacement = msg;
                    replacement.msgType = MsgType::ADD;
                    replacement.symbolPacked = old.symbolPacked;
                    replacement.side = static_cast<uint8_t>(old.side);
                    replacement.orderType = static_cast<uint8_t>(OrderType::LIMIT);
                    add(replacement);
                }
                break;
            }
        }
    }

    void add(const FeedMessage& msg) {
        RestingOrder incoming;
        incoming.orderId = msg.orderId;
        incoming.symbolPacked = msg.symbolPacked;
        incoming.price = msg.price;
        incoming.qty = msg.qty;
        incoming.side = static_cast<Side>(msg.side);
        incoming.sequence = msg.sequence;

        match(incoming, static_cast<OrderType>(msg.orderType), msg.sequence);
        if (incoming.remainingQty() > 0 &&
            static_cast<OrderType>(msg.orderType) == OrderType::LIMIT) {
            m_book.push_back(incoming);
        }
    }

    void match(RestingOrder& incoming, OrderType type, uint64_t ts) {
        while (incoming.remainingQty() > 0) {
            auto best = bestOpposite(incoming.side, incoming.price, type);
            if (best == m_book.end()) return;

            const uint32_t qty = std::min(incoming.remainingQty(), best->remainingQty());
            best->filledQty += qty;
            incoming.filledQty += qty;

            Fill f;
            if (incoming.side == Side::BUY) {
                f.buyOrderId = incoming.orderId;
                f.sellOrderId = best->orderId;
            } else {
                f.buyOrderId = best->orderId;
                f.sellOrderId = incoming.orderId;
            }
            f.price = best->price;
            f.qty = qty;
            f.timestamp = ts;
            f.symbolPacked = incoming.symbolPacked;
            m_fills.push_back(f);

            if (best->remainingQty() == 0) m_book.erase(best);
        }
    }

    std::vector<RestingOrder>::iterator bestOpposite(Side incomingSide,
                                                     uint32_t incomingPrice,
                                                     OrderType type) {
        auto best = m_book.end();
        for (auto it = m_book.begin(); it != m_book.end(); ++it) {
            if (it->side == incomingSide || it->remainingQty() == 0) continue;
            if (type != OrderType::MARKET) {
                const bool crosses = incomingSide == Side::BUY
                    ? it->price <= incomingPrice
                    : it->price >= incomingPrice;
                if (!crosses) continue;
            }
            if (best == m_book.end() || betterPriceTime(incomingSide, *it, *best)) {
                best = it;
            }
        }
        return best;
    }

    static bool betterPriceTime(Side incomingSide,
                                const RestingOrder& a,
                                const RestingOrder& b) noexcept {
        if (a.price != b.price) {
            return incomingSide == Side::BUY ? a.price < b.price : a.price > b.price;
        }
        return a.sequence < b.sequence;
    }

    bool eraseOrder(uint64_t orderId, RestingOrder* erased = nullptr) {
        auto it = std::find_if(m_book.begin(), m_book.end(), [&](const RestingOrder& o) {
            return o.orderId == orderId;
        });
        if (it == m_book.end()) return false;
        if (erased) *erased = *it;
        m_book.erase(it);
        return true;
    }

    std::vector<RestingOrder> m_book;
    std::vector<Fill> m_fills;
};

} // namespace chronobook
