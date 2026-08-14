// Layer 2 — property tests. Phase 1.
//
// Generate random valid order sequences from a recorded seed, apply them to the
// book, and assert every invariant after *every single operation* — not at the
// end of the run. An invariant checked only at the end tells you something
// broke, not which operation broke it.
//
// The invariants, from ARCHITECTURE.md and RESULTS.md:
//
//   1. The book never crosses: best_bid < best_ask whenever both sides are
//      non-empty.
//   2. Conservation: quantity added == resting + traded + cancelled.
//   3. Every id in the lookup map resolves to an order actually present in a
//      level.
//   4. Every level's cached aggregate equals the sum of its orders'
//      quantities. This is the one that catches aggregate drift on partial
//      fills, which is the bug this layer exists for.
//   5. No active level is empty. A ghost level makes best-price queries wrong.
//
// Definition of done for phase 1 is 10,000+ sequences with all five holding.
// Print the seed on failure — a property test you cannot replay is a property
// test you cannot debug.

#include <catch2/catch_test_macros.hpp>

// TODO(phase 1): tests go here once NaiveBook exists.
