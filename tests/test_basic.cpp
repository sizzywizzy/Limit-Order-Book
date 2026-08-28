// Layer 1 — unit tests.
//
// Three tiers here, cheapest first: the value types (the two places side
// asymmetry is allowed to exist), the phase 2 components against small
// deterministic cases (TieredBitmap, IdIndex), and the full unit scenario set
// from tests/scenarios.hpp run against BOTH engines — every order type plus
// the edge cases that actually break engines: cancelling a non-existent
// order, exactly consuming a level, consuming a whole side, zero quantity,
// duplicate ids, band violations, capacity exhaustion, and every modify
// semantic.

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <type_traits>

#include "ob/bitmap.hpp"
#include "ob/book_fast.hpp"
#include "ob/book_naive.hpp"
#include "ob/events.hpp"
#include "ob/id_index.hpp"
#include "ob/order.hpp"
#include "ob/types.hpp"
#include "scenarios.hpp"

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

TEST_CASE("events are flat comparable values", "[events]") {
    // Differential testing compares whole streams by value; the event type
    // must stay a plain value for that to remain meaningful.
    STATIC_REQUIRE(std::is_trivially_copyable_v<ob::Event>);
    REQUIRE(ob::Event::accepted(1, ob::Side::Buy, 100, 5) ==
            ob::Event::accepted(1, ob::Side::Buy, 100, 5));
    REQUIRE_FALSE(ob::Event::accepted(1, ob::Side::Buy, 100, 5) ==
                  ob::Event::accepted(1, ob::Side::Buy, 100, 6));
}

TEST_CASE("tiered bitmap tracks bits across word boundaries", "[bitmap]") {
    ob::TieredBitmap bm(4096);

    SECTION("empty") {
        REQUIRE(bm.first() == ob::npos32);
        REQUIRE(bm.last() == ob::npos32);
        REQUIRE_FALSE(bm.any());
    }

    SECTION("single bit") {
        bm.set(70);
        REQUIRE(bm.first() == 70);
        REQUIRE(bm.last() == 70);
        REQUIRE(bm.test(70));
        REQUIRE(bm.next_above(70) == ob::npos32);
        REQUIRE(bm.next_below(70) == ob::npos32);
        bm.clear(70);
        REQUIRE_FALSE(bm.any());
    }

    SECTION("word boundaries at 63/64/65 and tier boundaries at 4095") {
        for (const std::uint32_t i : {0u, 63u, 64u, 65u, 4095u}) {
            bm.set(i);
        }
        REQUIRE(bm.first() == 0);
        REQUIRE(bm.last() == 4095);
        REQUIRE(bm.next_above(0) == 63);
        REQUIRE(bm.next_above(63) == 64);
        REQUIRE(bm.next_above(64) == 65);
        REQUIRE(bm.next_above(65) == 4095);
        REQUIRE(bm.next_below(4095) == 65);
        REQUIRE(bm.next_below(64) == 63);
        bm.clear(64);
        REQUIRE(bm.next_above(63) == 65);
        REQUIRE(bm.next_below(65) == 63);
    }

    SECTION("clearing the last bit in a word clears the summary path") {
        bm.set(128);  // alone in its l0 word
        bm.set(4000);
        bm.clear(128);
        REQUIRE(bm.first() == 4000);
        REQUIRE(bm.next_below(4000) == ob::npos32);
    }
}

TEST_CASE("id index inserts, finds and erases without tombstone drift",
          "[idindex]") {
    ob::IdIndex ix(64);
    for (ob::OrderId id = 1; id <= 64; ++id) {
        ix.insert(id, static_cast<std::uint32_t>(id * 10));
    }
    REQUIRE(ix.size() == 64);
    for (ob::OrderId id = 1; id <= 64; ++id) {
        REQUIRE(ix.find(id) == id * 10);
    }
    // Erase every other key, then verify the survivors are all still
    // reachable — this is what backward-shift deletion must preserve.
    for (ob::OrderId id = 2; id <= 64; id += 2) {
        REQUIRE(ix.erase(id));
    }
    REQUIRE(ix.size() == 32);
    for (ob::OrderId id = 1; id <= 64; ++id) {
        if (id % 2 == 1) {
            REQUIRE(ix.find(id) == id * 10);
        } else {
            REQUIRE(ix.find(id) == ob::npos32);
            REQUIRE_FALSE(ix.erase(id));
        }
    }
    REQUIRE(ix.find(0) == ob::npos32);
}

TEMPLATE_TEST_CASE("engine unit scenarios", "[book]",
                   ob::FastBook<ob::VectorSink>,
                   ob::NaiveBook<ob::VectorSink>) {
    const char* engine =
        std::is_same_v<TestType, ob::FastBook<ob::VectorSink>> ? "fast"
                                                               : "naive";
    obtest::unit_scenarios<TestType>(
        [](bool ok, const std::string& what) {
            INFO(what);
            REQUIRE(ok);
        },
        engine);
}
