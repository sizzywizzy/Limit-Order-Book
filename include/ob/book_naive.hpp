#pragma once

// Phase 1 — the reference engine.
//
// std::map<Price, Level> per side, std::list<Order> as the FIFO at each level,
// std::unordered_map<OrderId, iterator> for cancel. Deliberately obvious and
// deliberately slow. It is never deleted: it is the correctness oracle for
// differential testing and the performance baseline for every speedup claim
// (DECISIONS.md 002).
//
// Build order for this file is the ten steps in the build manual, phase 1.
// The public interface defined here is the one the phase 2 engine must also
// satisfy, so settle it before writing book_fast.hpp.

#include "ob/order.hpp"
#include "ob/types.hpp"

namespace ob {

// TODO(phase 1): class NaiveBook.

}  // namespace ob
