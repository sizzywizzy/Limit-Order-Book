// Layer 1 — unit tests.
//
// One case per order type, plus the edge cases that actually break engines:
// cancelling an order that does not exist, an order that exactly consumes a
// level, an order that consumes an entire side, zero quantity, a duplicate id,
// and a self-trade.
//
// Until phase 1 lands there is no book to test, so what follows exercises the
// value types. These are not filler: `opposite` and `is_better_or_equal` are
// the two places side asymmetry is allowed to exist, and every price
// comparison in both engines routes through the latter.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <type_traits>

#include "ob/order.hpp"
#include "ob/types.hpp"

TEST_CASE("price is a signed 64-bit integer", "[types]") {
    // DECISIONS.md 001. Signed, because price differences are meaningful and
    // unsigned subtraction wraps.
    STATIC_REQUIRE(std::is_same_v<ob::Price, std::int64_t>);
    STATIC_REQUIRE(std::is_signed_v<ob::Price>);
    STATIC_REQUIRE_FALSE(std::is_floating_point_v<ob::Price>);
}

TEST_CASE("side and order type are one byte", "[types]") {
    // Order layout is cache-sensitive in phase 2; these must not silently
    // widen to int.
    STATIC_REQUIRE(sizeof(ob::Side) == 1);
    STATIC_REQUIRE(sizeof(ob::OrderType) == 1);
}

TEST_CASE("opposite side round-trips", "[types]") {
    STATIC_REQUIRE(ob::opposite(ob::Side::Buy) == ob::Side::Sell);
    STATIC_REQUIRE(ob::opposite(ob::Side::Sell) == ob::Side::Buy);
    STATIC_REQUIRE(ob::opposite(ob::opposite(ob::Side::Buy)) == ob::Side::Buy);
}

TEST_CASE("better price is higher for a buyer and lower for a seller", "[types]") {
    using ob::is_better_or_equal;
    using ob::Side;

    SECTION("buyer") {
        REQUIRE(is_better_or_equal(Side::Buy, 101, 100));
        REQUIRE(is_better_or_equal(Side::Buy, 100, 100));
        REQUIRE_FALSE(is_better_or_equal(Side::Buy, 99, 100));
    }

    SECTION("seller") {
        REQUIRE(is_better_or_equal(Side::Sell, 99, 100));
        REQUIRE(is_better_or_equal(Side::Sell, 100, 100));
        REQUIRE_FALSE(is_better_or_equal(Side::Sell, 101, 100));
    }

    SECTION("equality is inclusive on both sides, so a limit order crosses at "
            "its own price") {
        REQUIRE(is_better_or_equal(Side::Buy, 100, 100));
        REQUIRE(is_better_or_equal(Side::Sell, 100, 100));
    }
}

TEST_CASE("a default-constructed order is inert", "[order]") {
    // Zero quantity means an accidentally default-constructed Order cannot be
    // mistaken for a live one.
    const ob::Order o{};
    REQUIRE(o.qty == 0u);
    REQUIRE(o.id == 0u);
    REQUIRE(o.type == ob::OrderType::Limit);
}
