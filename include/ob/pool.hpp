#pragma once

// Phase 2 — pre-allocated order pool with a free list threaded through the
// unused slots, so that acquire and release are both O(1) and neither
// allocates (ARCHITECTURE.md 3.3).
//
// Two things must be decided and written into DECISIONS.md 005 before this is
// implemented: how capacity is chosen, and what happens on exhaustion. Reject
// and grow are both defensible; failing silently is not.

#include <cstddef>

#include "ob/order.hpp"

namespace ob {

// TODO(phase 2): class OrderPool.

}  // namespace ob
