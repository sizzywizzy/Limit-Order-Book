#pragma once

#include <cstdint>

#include "ob/types.hpp"

// The instrument this book trades: one European option series per book.
//
// The matching core is deliberately instrument-agnostic — price-time priority
// does not care what the price is a price of. What the instrument type buys is
// scope control: European exercise means nothing can happen to a position
// intraday except trading, so the book needs no exercise, assignment or
// early-close message paths at all. That absence is a feature and is enforced
// by the type system: ExerciseStyle has exactly one value, so an American
// series is unrepresentable rather than merely unhandled. See DECISIONS.md 009.

namespace ob {

enum class OptionRight : std::uint8_t {
    Call,
    Put,
};

enum class ExerciseStyle : std::uint8_t {
    European,  // exercisable at expiry only — the only representable style
};

// One listed option series. Identity, not behaviour: the book stores this for
// reporting and routing; matching never reads it.
//
// A multi-series venue instantiates one book per series (README, Limitations —
// single instrument per book). Mapping human symbology ("NIFTY 24000 CE
// 25-Sep-2026") to and from these fields happens at the API boundary, in one
// place, exactly like the tick/display price conversion.
struct OptionSeries {
    // Numeric id of the underlying, assigned by the boundary layer.
    std::uint32_t underlying_id{0};

    // Expiry date as YYYYMMDD. European style: settlement happens at this
    // date and nowhere else, which is why the book never needs to know
    // today's date.
    std::uint32_t expiry{0};

    // Strike in the underlying's ticks. Distinct grid from the premium ticks
    // the book itself quotes in — the book's price axis is the option
    // premium, whose base and tick count are constructor parameters.
    Price strike{0};

    OptionRight right{OptionRight::Call};

    ExerciseStyle style{ExerciseStyle::European};

    friend bool operator==(const OptionSeries&, const OptionSeries&) = default;
};

}  // namespace ob
