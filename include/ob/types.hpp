#pragma once

#include <cstdint>

// Core value types shared by every engine implementation.
//
// See DECISIONS.md 001 for why prices are integers rather than floating point.

namespace ob {

// Price in ticks, counted from a base price that each book documents at
// construction. A tick is the instrument's minimum price increment; the
// conversion between ticks and a display price happens at the API boundary and
// in exactly one place.
//
// Signed, because the difference of two prices is a meaningful quantity and
// unsigned subtraction wraps.
using Price = std::int64_t;

// Quantity in lots, where one lot is the instrument's minimum tradeable unit.
// Never fractional: an order for half a lot is not representable and is
// rejected at the boundary.
using Quantity = std::uint64_t;

// Client-assigned order identifier. Uniqueness among live orders is the
// caller's responsibility; the engine rejects a duplicate against a live order
// rather than silently overwriting it.
using OrderId = std::uint64_t;

enum class Side : std::uint8_t {
    Buy,
    Sell,
};

enum class OrderType : std::uint8_t {
    Limit,   // rests any residual quantity at its limit price
    Market,  // no limit price; residual is discarded
    IOC,     // immediate-or-cancel: fill what is available, discard the rest
    FOK,     // fill-or-kill: fill entirely or reject entirely
};

// Bids and asks are mirrored rather than duplicated: the matching logic is
// written once and parameterised on side. See DECISIONS.md 002.
constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// True if `price` is at least as good as `reference` for a participant on
// `side` — higher is better for a buyer, lower for a seller. This is the single
// definition of "better price" used by every price comparison in the engine.
constexpr bool is_better_or_equal(Side side, Price price, Price reference) noexcept {
    return side == Side::Buy ? price >= reference : price <= reference;
}

}  // namespace ob
