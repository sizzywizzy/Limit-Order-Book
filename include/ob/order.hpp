#pragma once

#include "ob/types.hpp"

namespace ob {

// The phase 1 order: a plain value type, owned by whatever container holds it.
//
// The phase 2 engine extends this with intrusive `prev` / `next` / `level`
// pointers so that an order can be unlinked in O(1) from a pointer alone
// (ARCHITECTURE.md 3.4). Those links are deliberately absent here — the
// reference engine must stay obvious, because its job is to be trusted rather
// than fast.
struct Order {
    OrderId   id{};
    Side      side{Side::Buy};
    OrderType type{OrderType::Limit};

    // Limit price in ticks. Unused for Market orders.
    Price price{};

    // Remaining quantity. Decremented as the order fills; an order reaching
    // zero is removed from the book.
    Quantity qty{};
};

}  // namespace ob
