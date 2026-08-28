// Layer 2 — property tests.
//
// Random valid order sequences from recorded seeds, applied to each engine,
// with every invariant asserted after *every single operation* — not at the
// end of the run. An invariant checked only at the end tells you something
// broke, not which operation broke it.
//
// The invariants (ARCHITECTURE.md, RESULTS.md), enforced by the combination
// of each book's validate() and the event-stream ledger in tests/harness.hpp:
//
//   1. The book never crosses: best_bid < best_ask when both sides non-empty.
//   2. Conservation: every accepted quantity is accounted for as resting,
//      traded, cancelled, or discarded — checked by replaying the event
//      stream into a ledger and reconciling against the book.
//   3. Every id resolves to an order actually resting in a level.
//   4. Every level's cached aggregate equals the sum of its orders'
//      quantities (the invariant that catches drift on partial fills).
//   5. No active level is empty.
//
// Failures print the seed and operation index — a property test you cannot
// replay is a property test you cannot debug.
//
// This layer also owns the two whole-engine claims that are not per-operation
// invariants: deterministic replay, and zero steady-state allocation for the
// fast engine (proved with a counting global allocator, not asserted).

#include "alloc_counter.hpp"  // global new/delete replacement: keep first

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"
#include "ob/book_fast.hpp"
#include "ob/book_naive.hpp"

namespace {
// CI-sized. The 10,000-sequence definition-of-done run is the [.slow] case.
constexpr std::size_t kSequences = 400;      // per engine, distinct seeds
constexpr std::size_t kOpsPerSequence = 250;
constexpr std::uint64_t kSeedBase = 1000;
}  // namespace

TEST_CASE("fast engine holds all invariants over randomized sequences",
          "[properties][fast]") {
    for (std::size_t s = 0; s < kSequences; ++s) {
        const std::string err =
            obtest::run_property_sequence<ob::FastBook<ob::VectorSink>>(
                kSeedBase + s, obtest::BookParams{}, kOpsPerSequence);
        INFO(err);
        REQUIRE(err.empty());
    }
}

TEST_CASE("naive engine holds all invariants over randomized sequences",
          "[properties][naive]") {
    for (std::size_t s = 0; s < kSequences; ++s) {
        const std::string err =
            obtest::run_property_sequence<ob::NaiveBook<ob::VectorSink>>(
                kSeedBase + s, obtest::BookParams{}, kOpsPerSequence);
        INFO(err);
        REQUIRE(err.empty());
    }
}

// Phase 1 definition of done asks for 10,000+ sequences; that budget belongs
// to a deliberate run, not every CI push. Hidden behind the [.slow] tag:
//     ./ob_test_properties "[slow]"
TEST_CASE("10,000 sequences per engine", "[.slow][properties]") {
    for (std::size_t s = 0; s < 10000; ++s) {
        std::string err =
            obtest::run_property_sequence<ob::FastBook<ob::VectorSink>>(
                kSeedBase + s, obtest::BookParams{}, 300);
        if (err.empty()) {
            err = obtest::run_property_sequence<ob::NaiveBook<ob::VectorSink>>(
                kSeedBase + s, obtest::BookParams{}, 300);
        }
        INFO(err);
        REQUIRE(err.empty());
    }
}

TEST_CASE("identical input produces an identical event stream",
          "[properties][determinism]") {
    const obtest::BookParams p;
    std::vector<obtest::Op> flow;
    obtest::FlowGen gen(2024, p);
    for (std::size_t i = 0; i < 50000; ++i) {
        flow.push_back(gen.next());
    }
    auto run = [&] {
        ob::FastBook<ob::VectorSink> book(p.base, p.num_ticks, p.capacity);
        for (const auto& op : flow) {
            obtest::apply(book, op);
        }
        return std::move(book.sink().events);
    };
    const auto first = run();
    const auto second = run();
    REQUIRE_FALSE(first.empty());
    REQUIRE(first == second);
}

TEST_CASE("fast engine allocates nothing in the steady state",
          "[properties][allocation]") {
    if (!obtest::alloc_counting_enabled) {
        SKIP("allocation counting disabled under this build (sanitizers)");
    }

    const obtest::BookParams p{1000, 2048, 4096};
    constexpr std::size_t kOps = 100000;
    std::vector<obtest::Op> flow;
    flow.reserve(kOps * 2);
    obtest::FlowGen gen(9001, p);
    for (std::size_t i = 0; i < kOps * 2; ++i) {
        flow.push_back(gen.next());
    }

    // NullSink: the engine's own behaviour, no test-side event recording.
    ob::FastBook<ob::NullSink> book(p.base, p.num_ticks, p.capacity);
    for (std::size_t i = 0; i < kOps; ++i) {  // warm-up half
        obtest::apply(book, flow[i]);
    }

    const std::uint64_t before = obtest::allocations();
    for (std::size_t i = kOps; i < flow.size(); ++i) {
        obtest::apply(book, flow[i]);
    }
    const std::uint64_t after = obtest::allocations();

    REQUIRE(after - before == 0);
}
