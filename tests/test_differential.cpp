// Layer 3 — differential tests.
//
// NaiveBook and FastBook consume identical random sequences and their entire
// output event streams must compare equal after every operation — not just
// the final book state, because two engines can agree on where they end up
// and disagree about how they got there. Observable state (best prices,
// depth snapshots, order counts) and both engines' structural invariants are
// cross-checked at regular checkpoints as well.
//
// This is the layer that licenses every claim the README makes about the
// optimised engine. The phase 2 definition of done — 1,000,000+ operations —
// lives behind the [.slow] tag for deliberate runs:
//
//     ./ob_test_differential "[slow]"
//
// Run this layer under the sanitised build too (cmake -DOB_SANITIZE=ON): the
// pool's use-after-free hazard (ARCHITECTURE.md 3.5) is exactly the kind of
// bug that produces identical output right up until it does not.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "harness.hpp"

TEST_CASE("naive and fast engines emit identical event streams",
          "[differential]") {
    for (const std::uint64_t seed : {1ull, 31337ull, 987654321ull}) {
        const std::string err =
            obtest::run_differential(seed, obtest::BookParams{}, 40000);
        INFO(err);
        REQUIRE(err.empty());
    }
}

TEST_CASE("differential across book geometries", "[differential]") {
    // Narrow band and tiny capacity force constant crossing, level churn and
    // capacity rejections; a wide band exercises the bitmap's upper tiers.
    struct Geometry {
        obtest::BookParams params;
        std::size_t ops;
    };
    const Geometry geometries[] = {
        {{1000, 64, 128}, 30000},    // cramped: heavy crossing and rejects
        {{500, 8192, 2048}, 30000},  // wide: sparse levels, deep bitmap
    };
    for (const auto& g : geometries) {
        const std::string err = obtest::run_differential(7, g.params, g.ops);
        INFO(err);
        REQUIRE(err.empty());
    }
}

TEST_CASE("1,000,000-operation differential", "[.slow][differential]") {
    const std::string err =
        obtest::run_differential(31337, obtest::BookParams{}, 1000000);
    INFO(err);
    REQUIRE(err.empty());
}
