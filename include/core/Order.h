#pragma once
// Order.h
//
// A single resting/incoming order. 48 bytes, intrusive list pointers first so
// the price-level FIFO never allocates a separate node.
//   - integer (fixed-point) prices, never double: matching needs exact equality
//   - symbol packed into a uint64 via memcpy (strict-aliasing safe)
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace chronobook {

enum class Side      : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType : uint8_t { LIMIT = 0, MARKET = 1, IOC = 2 };

struct alignas(8) Order {
    Order*    next{nullptr};      // intrusive FIFO - next at same price level
    Order*    prev{nullptr};      // intrusive FIFO - prev at same price level
    uint64_t  orderId{0};
    uint64_t  symbolPacked{0};    // up to 8 ASCII chars packed into 64 bits
    uint32_t  price{0};           // fixed-point ticks (10050 == $100.50 @ tick 0.01)
    uint32_t  qty{0};             // original order quantity
    uint32_t  filledQty{0};       // quantity matched so far
    Side      side{Side::BUY};
    OrderType type{OrderType::LIMIT};
    char      padding[2]{0, 0};   // explicit - keeps struct exactly 48 bytes

    void setSymbol(std::string_view sym) noexcept {
        symbolPacked = 0;
        const size_t len = sym.length() > 8 ? 8 : sym.length();
        std::memcpy(&symbolPacked, sym.data(), len);  // memcpy = no aliasing UB
    }
    std::string getSymbol() const {
        char buf[9]{0};
        std::memcpy(buf, &symbolPacked, 8);
        return std::string(buf);
    }
    // clamp to 0 so an accidental overfill can never wrap an unsigned to ~4e9
    uint32_t remainingQty() const noexcept {
        return (filledQty >= qty) ? 0u : (qty - filledQty);
    }
    bool isFilled() const noexcept { return filledQty >= qty; }
};

static_assert(sizeof(Order) == 48, "Order must be exactly 48 bytes");

} // namespace chronobook
