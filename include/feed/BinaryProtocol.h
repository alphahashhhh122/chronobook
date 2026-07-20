#pragma once
// BinaryProtocol.h
//
// A tiny, fixed-width binary wire format for the order feed. One message type
// on the wire: FeedMessage (40 bytes, explicitly laid out, no compiler padding
// surprises). We deliberately do NOT do ITCH/PITCH - a custom protocol keeps
// the focus on layout + serialization, not network-format trivia.
//
// Serialization rule: whole-struct memcpy to/from a byte buffer. memcpy is the
// only strict-aliasing-safe way to reinterpret bytes as a trivially-copyable
// struct (and vice-versa). We never reinterpret_cast a char* to FeedMessage*.
//
// Endianness caveat (kept explicit): this format is host-endian. It is a
// same-machine replay protocol, so that's fine; a cross-host feed would byte-
// swap each field to a fixed endianness (e.g. little-endian) on the wire.
#include "core/Order.h"
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace chronobook {

enum class MsgType : uint8_t { ADD = 0, CANCEL = 1, MODIFY = 2 };

// Layout (verified by static_assert):
//   sequence     8   monotonic feed sequence number (also the fill timestamp)
//   orderId      8   target order id
//   symbolPacked 8   8-char symbol for every message. CANCEL and MODIFY carry
//                    the original order symbol so a symbol-sharded router can
//                    send the request to the worker that owns the order.
//   price        4   ticks (ADD/MODIFY new price); 0 for CANCEL / MARKET
//   qty          4   quantity (ADD original qty / MODIFY new qty); 0 for CANCEL
//   msgType      1   ADD / CANCEL / MODIFY
//   side         1   Side (ADD only)
//   orderType    1   OrderType (ADD only)
//   _pad         5   explicit pad -> 40 bytes, deterministic on every compiler
struct FeedMessage {
    uint64_t sequence{0};
    uint64_t orderId{0};
    uint64_t symbolPacked{0};
    uint32_t price{0};
    uint32_t qty{0};
    MsgType  msgType{MsgType::ADD};
    uint8_t  side{0};
    uint8_t  orderType{0};
    uint8_t  _pad[5]{0, 0, 0, 0, 0};
};

static_assert(sizeof(FeedMessage) == 40, "FeedMessage must be 40 bytes");
static_assert(std::is_trivially_copyable_v<FeedMessage>,
              "FeedMessage must be trivially copyable for memcpy serialization");

inline constexpr size_t kFeedMessageSize = sizeof(FeedMessage);

// Serialize one message into `dst` (must have >= kFeedMessageSize bytes).
inline void encodeMessage(const FeedMessage& m, std::byte* dst) noexcept {
    std::memcpy(dst, &m, kFeedMessageSize);
}

// Deserialize one message from `src` (must have >= kFeedMessageSize bytes).
inline FeedMessage decodeMessage(const std::byte* src) noexcept {
    FeedMessage m;
    std::memcpy(&m, src, kFeedMessageSize);
    return m;
}

inline FeedMessage makeAdd(uint64_t seq, uint64_t id, Side side, OrderType type,
                           uint32_t price, uint32_t qty, uint64_t symbolPacked) noexcept {
    FeedMessage m;
    m.sequence = seq; m.orderId = id; m.symbolPacked = symbolPacked;
    m.price = price; m.qty = qty;
    m.msgType = MsgType::ADD;
    m.side = static_cast<uint8_t>(side);
    m.orderType = static_cast<uint8_t>(type);
    return m;
}
inline FeedMessage makeCancel(uint64_t seq, uint64_t id, uint64_t symbolPacked) noexcept {
    FeedMessage m;
    m.sequence = seq; m.orderId = id; m.symbolPacked = symbolPacked;
    m.msgType = MsgType::CANCEL;
    return m;
}
inline FeedMessage makeModify(uint64_t seq, uint64_t id, uint32_t newPrice,
                               uint32_t newQty, uint64_t symbolPacked) noexcept {
    FeedMessage m;
    m.sequence = seq; m.orderId = id; m.symbolPacked = symbolPacked;
    m.price = newPrice; m.qty = newQty;
    m.msgType = MsgType::MODIFY;
    return m;
}

inline bool isValidMsgType(MsgType type) noexcept {
    return type == MsgType::ADD || type == MsgType::CANCEL || type == MsgType::MODIFY;
}

inline bool isValidSide(uint8_t side) noexcept {
    return side == static_cast<uint8_t>(Side::BUY) ||
           side == static_cast<uint8_t>(Side::SELL);
}

inline bool isValidOrderType(uint8_t type) noexcept {
    return type == static_cast<uint8_t>(OrderType::LIMIT) ||
           type == static_cast<uint8_t>(OrderType::MARKET) ||
           type == static_cast<uint8_t>(OrderType::IOC);
}

inline bool isValidFeedMessage(const FeedMessage& m) noexcept {
    if (!isValidMsgType(m.msgType) || m.orderId == 0) return false;
    switch (m.msgType) {
        case MsgType::ADD:
            if (!isValidSide(m.side) || !isValidOrderType(m.orderType) || m.qty == 0)
                return false;
            if (m.orderType != static_cast<uint8_t>(OrderType::MARKET) && m.price == 0)
                return false;
            return true;
        case MsgType::CANCEL:
            return true;
        case MsgType::MODIFY:
            return m.price != 0 && m.qty != 0;
    }
    return false;
}

} // namespace chronobook
