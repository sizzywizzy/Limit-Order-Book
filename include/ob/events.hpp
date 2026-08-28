#pragma once

#include <concepts>
#include <cstdint>
#include <vector>

#include "ob/types.hpp"

// The deterministic event stream every engine emits.
//
// Both engines must produce value-identical event sequences for identical
// input — that property is what the differential test layer checks, and it is
// why the Event type is a fixed-layout value with defaulted equality rather
// than a class hierarchy. See DECISIONS.md 008.

namespace ob {

enum class EventType : std::uint8_t {
    Accepted,   // an order (or its residual after matching) came to rest
    Trade,      // a fill; carries taker id, maker id, price, quantity
    Reduced,    // in-place quantity reduction; queue position preserved
    Cancelled,  // a resting order was removed by request (or by modify)
    Rejected,   // an operation was refused; `reason` says why
};

enum class RejectReason : std::uint8_t {
    None,               // not a rejection
    InvalidId,          // id 0 is reserved as "no order"
    DuplicateId,        // id already names a live resting order
    ZeroQuantity,       // quantity must be at least one lot
    PriceOutOfRange,    // outside [base, base + num_ticks) — DECISIONS.md 003
    CapacityExhausted,  // book is at its configured order capacity
    FokUnfillable,      // fill-or-kill could not fill entirely
    UnknownOrder,       // cancel or modify named an id that is not resting
};

// Fixed-size value type. Every field is written by every factory, including
// the explicit padding, so value comparison is total and deterministic.
//
// Field use by event type:
//
//   Accepted    id, side, price, qty = quantity that came to rest
//   Trade       id = taker, maker_id = maker, side = taker side,
//               price = maker's price, qty = fill quantity
//   Reduced     id, side, price, qty = new remaining quantity
//   Cancelled   id, side, price, qty = remaining quantity at removal
//   Rejected    id, reason, and an echo of the request's side/price/qty
//               (side is Buy when the request named an unknown id and the
//               true side cannot be known)
struct Event {
    EventType     type{EventType::Accepted};
    Side          side{Side::Buy};
    RejectReason  reason{RejectReason::None};
    std::uint8_t  pad8_{0};
    std::uint32_t pad32_{0};
    OrderId       id{0};
    OrderId       maker_id{0};
    Price         price{0};
    Quantity      qty{0};

    friend bool operator==(const Event&, const Event&) = default;

    static constexpr Event accepted(OrderId id, Side side, Price price,
                                    Quantity qty) noexcept {
        return Event{EventType::Accepted, side, RejectReason::None,
                     0, 0, id, 0, price, qty};
    }
    static constexpr Event trade(OrderId taker, OrderId maker, Side taker_side,
                                 Price price, Quantity qty) noexcept {
        return Event{EventType::Trade, taker_side, RejectReason::None,
                     0, 0, taker, maker, price, qty};
    }
    static constexpr Event reduced(OrderId id, Side side, Price price,
                                   Quantity remaining) noexcept {
        return Event{EventType::Reduced, side, RejectReason::None,
                     0, 0, id, 0, price, remaining};
    }
    static constexpr Event cancelled(OrderId id, Side side, Price price,
                                     Quantity remaining) noexcept {
        return Event{EventType::Cancelled, side, RejectReason::None,
                     0, 0, id, 0, price, remaining};
    }
    static constexpr Event rejected(OrderId id, Side side, RejectReason why,
                                    Price price, Quantity qty) noexcept {
        return Event{EventType::Rejected, side, why, 0, 0, id, 0, price, qty};
    }
};

// Engines are parameterised on the sink at compile time so that event
// emission costs a direct (usually inlined) call, not a virtual dispatch,
// and so that the no-op sink vanishes entirely under optimisation.
template <typename S>
concept EventSink = requires(S s, const Event& e) {
    { s(e) };
};

// The production default: emits nothing, costs nothing.
struct NullSink {
    constexpr void operator()(const Event&) const noexcept {}
};

// Test and replay sink: records the full stream for comparison. Allocates —
// which is fine, because it exists for the test layers, not the hot path.
struct VectorSink {
    std::vector<Event> events;
    void operator()(const Event& e) { events.push_back(e); }
};

static_assert(EventSink<NullSink>);
static_assert(EventSink<VectorSink>);

}  // namespace ob
