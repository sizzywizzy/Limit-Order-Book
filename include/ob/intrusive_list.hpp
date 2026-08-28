#pragma once

#include <cassert>
#include <concepts>
#include <cstdint>

#include "ob/types.hpp"

// Intrusive doubly-linked FIFO over pool slots.
//
// The prev/next links live inside the pooled order itself, not in a container
// node, so unlinking is four writes from a slot index with no search, no
// lookup and no deallocation (ARCHITECTURE.md 3.4). This is the cancel path,
// and the cancel path is the point of the whole project.
//
// Links are 32-bit slot indices into the owning SlotPool rather than raw
// pointers (DECISIONS.md 004): half the size of a pointer, so twice as many
// links per cache line, and stable under anything that moves the storage.
// The cost of the whole approach is that order lifetime is manual — the
// ordering hazard in ARCHITECTURE.md 3.5 (erase from the id index before
// releasing to the pool) is a consequence of exactly this trade.

namespace ob {

template <typename T>
concept IntrusiveNode = requires(T t, std::uint32_t v) {
    { t.prev } -> std::convertible_to<std::uint32_t>;
    { t.next } -> std::convertible_to<std::uint32_t>;
    t.prev = v;
    t.next = v;
};

// Head/tail of one FIFO. The storage (a SlotPool) is passed to each operation
// rather than stored, so a book with thousands of levels carries eight bytes
// per level, not a container per level.
struct IndexFifo {
    std::uint32_t head{npos32};
    std::uint32_t tail{npos32};

    [[nodiscard]] bool empty() const noexcept { return head == npos32; }

    // Append at the tail: arrivals queue behind existing orders at the same
    // price. This is the "time" in price-time priority.
    template <typename Storage>
    void push_back(Storage& s, std::uint32_t i) noexcept
        requires IntrusiveNode<std::remove_reference_t<decltype(s[i])>>
    {
        auto& node = s[i];
        node.prev = tail;
        node.next = npos32;
        if (tail == npos32) {
            head = i;
        } else {
            s[tail].next = i;
        }
        tail = i;
    }

    // O(1) removal from anywhere in the queue, given only the slot index.
    // No search — this is why cancel costs what it costs.
    template <typename Storage>
    void unlink(Storage& s, std::uint32_t i) noexcept
        requires IntrusiveNode<std::remove_reference_t<decltype(s[i])>>
    {
        auto& node = s[i];
        const std::uint32_t p = node.prev;
        const std::uint32_t n = node.next;
        if (p == npos32) {
            assert(head == i);
            head = n;
        } else {
            s[p].next = n;
        }
        if (n == npos32) {
            assert(tail == i);
            tail = p;
        } else {
            s[n].prev = p;
        }
        node.prev = npos32;
        node.next = npos32;
    }
};

}  // namespace ob
