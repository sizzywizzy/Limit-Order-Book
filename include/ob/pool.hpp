#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ob/types.hpp"

// Pre-allocated slot pool with a free list threaded through the unused slots,
// so that acquire and release are both O(1) and neither allocates
// (ARCHITECTURE.md 3.3, DECISIONS.md 005).
//
// Slots are addressed by 32-bit index, not pointer. The free list reuses the
// `next` link every poolable type already carries for its intrusive FIFO, so
// the pool costs no additional memory per slot.
//
// Capacity is fixed at construction. Exhaustion is the caller's to handle —
// acquire() returns npos32 and the engine turns that into an explicit
// CapacityExhausted rejection rather than growing (DECISIONS.md 005: growth
// would allocate on the hot path, precisely the thing the pool exists to
// remove, and an order book at capacity is an operational fault to surface
// loudly, not to paper over).
//
// Construction value-initialises every slot, which touches every page — the
// pool is pre-faulted as a side effect, so the first thousand operations do
// not pay page-fault latency.

namespace ob {

template <typename T>
concept FreeListNode = requires(T t, std::uint32_t v) {
    { t.next } -> std::convertible_to<std::uint32_t>;
    t.next = v;
};

template <FreeListNode T>
class SlotPool {
public:
    explicit SlotPool(std::size_t capacity) : slots_(capacity) {
        assert(capacity < npos32);
        for (std::size_t i = 0; i < capacity; ++i) {
            slots_[i].next = (i + 1 < capacity)
                                 ? static_cast<std::uint32_t>(i + 1)
                                 : npos32;
        }
        free_head_ = capacity ? 0 : npos32;
    }

    // O(1). Returns npos32 when exhausted; never allocates.
    [[nodiscard]] std::uint32_t acquire() noexcept {
        if (free_head_ == npos32) {
            return npos32;
        }
        const std::uint32_t i = free_head_;
        free_head_ = slots_[i].next;
        ++in_use_;
        return i;
    }

    // O(1). The slot's contents are dead the moment this returns — the
    // engine erases the id-index entry *before* calling this
    // (ARCHITECTURE.md 3.5).
    void release(std::uint32_t i) noexcept {
        assert(i < slots_.size());
        assert(in_use_ > 0);
        slots_[i].next = free_head_;
        free_head_ = i;
        --in_use_;
    }

    [[nodiscard]] T& operator[](std::uint32_t i) noexcept {
        assert(i < slots_.size());
        return slots_[i];
    }
    [[nodiscard]] const T& operator[](std::uint32_t i) const noexcept {
        assert(i < slots_.size());
        return slots_[i];
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
    [[nodiscard]] std::size_t in_use() const noexcept { return in_use_; }

private:
    std::vector<T> slots_;  // sized once at construction, never resized
    std::uint32_t free_head_{npos32};
    std::size_t in_use_{0};
};

}  // namespace ob
