#pragma once

// Phase 2 — intrusive doubly-linked FIFO.
//
// The links live inside Order rather than in a container node, so unlinking is
// four pointer writes from an Order* with no search, no lookup and no
// deallocation (ARCHITECTURE.md 3.4). This is the cancel path, and the cancel
// path is the point of the whole project.
//
// The cost is that Order lifetime becomes manual. The ordering hazard in
// ARCHITECTURE.md 3.5 — erase from the id map before releasing to the pool —
// is a consequence of exactly this trade, and is what the ASan build in CI is
// there to catch.

namespace ob {

// TODO(phase 2): template <auto PrevPtr, auto NextPtr> class IntrusiveList.

}  // namespace ob
