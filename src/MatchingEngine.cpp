#include "matching/MatchingEngine.h"
#include "infra/CompilerHints.h"

#include <algorithm>

namespace chronobook {

void MatchingEngine::processOrder(Order* order, uint64_t sequenceNum) {
    if (CB_UNLIKELY(!order)) return;
    if (CB_UNLIKELY(m_book.containsOrder(order->orderId))) {
        m_pool.deallocate(order);
        return;
    }
    m_currentSeq = sequenceNum;
    switch (order->type) {
        case OrderType::LIMIT:  matchLimit(order);  break;
        case OrderType::MARKET: matchMarket(order); break;
        case OrderType::IOC:    matchIOC(order);    break;
    }
}

bool MatchingEngine::cancelOrder(uint64_t orderId) {
    Order* o = m_book.cancelOrder(orderId);
    if (CB_UNLIKELY(!o)) return false;
    m_pool.deallocate(o);
    return true;
}

bool MatchingEngine::modifyOrder(uint64_t orderId, uint32_t newPrice,
                                 uint32_t newQty, uint64_t sequenceNum) {
    Order* o = m_book.cancelOrder(orderId);
    if (CB_UNLIKELY(!o)) return false;

    o->price = newPrice;
    o->qty = newQty;
    o->filledQty = 0;
    o->type = OrderType::LIMIT;
    processOrder(o, sequenceNum);
    return true;
}

std::vector<Fill> MatchingEngine::drainFills() {
    std::vector<Fill> out = std::move(m_fills);
    m_fills.clear();
    m_fills.reserve(1u << 16);
    return out;
}

void MatchingEngine::drainFillsInto(std::vector<Fill>& out) {
    out.clear();
    out.swap(m_fills);
    if (m_fills.capacity() < (1u << 16)) m_fills.reserve(1u << 16);
}

void MatchingEngine::executeTrade(Order* resting, Order* incoming,
                                  uint32_t matchPrice, uint32_t matchQty) {
    resting->filledQty += matchQty;
    incoming->filledQty += matchQty;

    Fill f;
    if (incoming->side == Side::BUY) {
        f.buyOrderId = incoming->orderId;
        f.sellOrderId = resting->orderId;
    } else {
        f.buyOrderId = resting->orderId;
        f.sellOrderId = incoming->orderId;
    }
    f.price = matchPrice;
    f.qty = matchQty;
    f.timestamp = m_currentSeq;
    f.symbolPacked = incoming->symbolPacked;
    m_fills.push_back(f);
}

void MatchingEngine::matchOneSide(Order* incoming, bool respectPrice) {
    const bool incomingIsBuy = (incoming->side == Side::BUY);

    while (!incoming->isFilled()) {
        uint32_t restPrice = 0;
        PriceLevel* level = nullptr;

        if (CB_LIKELY(incomingIsBuy)) {
            restPrice = m_book.getBestAsk();
            if (CB_UNLIKELY(restPrice == 0)) break;
            level = m_book.getAskLevel(restPrice);
        } else {
            restPrice = m_book.getBestBid();
            if (CB_UNLIKELY(restPrice == 0)) break;
            level = m_book.getBidLevel(restPrice);
        }
        if (CB_UNLIKELY(!level)) break;

        if (respectPrice) {
            const bool crosses = incomingIsBuy
                ? (restPrice <= incoming->price)
                : (restPrice >= incoming->price);
            if (CB_UNLIKELY(!crosses)) break;
        }

        Order* resting = level->front();
        prefetchRead(resting ? resting->next : nullptr);
        const uint32_t matchQty =
            std::min(incoming->remainingQty(), resting->remainingQty());

        executeTrade(resting, incoming, restPrice, matchQty);
        level->decreaseTotalQty(matchQty);

        if (CB_UNLIKELY(resting->isFilled())) {
            m_book.removeOrder(resting);
            m_pool.deallocate(resting);
        }
    }
}

void MatchingEngine::matchLimit(Order* order) {
    matchOneSide(order, true);
    if (CB_LIKELY(!order->isFilled())) {
        if (CB_UNLIKELY(!m_book.addOrder(order))) m_pool.deallocate(order);
    } else {
        m_pool.deallocate(order);
    }
}

void MatchingEngine::matchMarket(Order* order) {
    matchOneSide(order, false);
    m_pool.deallocate(order);
}

void MatchingEngine::matchIOC(Order* order) {
    matchOneSide(order, true);
    m_pool.deallocate(order);
}

} // namespace chronobook
