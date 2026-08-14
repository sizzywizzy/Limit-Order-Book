#pragma once

// Phase 2 — the optimised engine.
//
// Same public interface as NaiveBook: flat level array indexed by tick offset,
// intrusive FIFOs, an order pool, and a direct id-to-pointer map. Every claim
// made about this engine is licensed by the differential test against
// NaiveBook, so do not start it until phase 1's definition of done is met.

#include "ob/intrusive_list.hpp"
#include "ob/order.hpp"
#include "ob/pool.hpp"
#include "ob/types.hpp"

namespace ob {

// TODO(phase 2): class FastBook.

}  // namespace ob
