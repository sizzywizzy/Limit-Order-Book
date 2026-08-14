// Layer 3 — differential tests. Phase 2.
//
// Run NaiveBook and FastBook over identical random sequences and compare their
// entire output event streams byte for byte. Not just the final book state:
// the streams, because two engines can agree on where they end up and disagree
// about how they got there.
//
// This is the layer that licenses every claim the README makes about the
// optimised engine. Phase 2's definition of done is 1,000,000+ operations with
// identical streams.
//
// Run this one under the sanitised build too:
//     cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON
// The pool's use-after-free hazard (ARCHITECTURE.md 3.5) is exactly the kind of
// bug that produces identical output right up until it does not.

#include <catch2/catch_test_macros.hpp>

// TODO(phase 2): tests go here once FastBook exists.
