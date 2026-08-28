#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ob/types.hpp"

// OrderId → pool-slot index, as one flat open-addressing table.
//
// Client-assigned ids are arbitrary 64-bit values, so direct array indexing
// is out and some hashing is unavoidable — but std::unordered_map buys that
// hashing with a node allocation per insert and a pointer chase per lookup.
// This table is a single contiguous array probed linearly: the common case is
// one cache line touched per lookup, and no operation ever allocates after
// construction.
//
// Sizing: the table holds at most the book's order capacity, and is sized to
// the next power of two of twice that, so load factor never exceeds 0.5 and
// probe sequences stay short. There is no rehash path at all — the capacity
// argument makes overflow unreachable, which is exactly the kind of "cannot
// happen by construction" the hot path wants.
//
// Deletion is backward-shift compaction rather than tombstones: tombstones
// make probe chains grow monotonically under the cancel-heavy flow this book
// is built for, which is a slow leak of exactly the latency the flat table
// exists to save. See DECISIONS.md 007.
//
// Key 0 marks an empty cell, which is why OrderId 0 is reserved as invalid at
// the engine boundary (rejected with RejectReason::InvalidId).

namespace ob {

class IdIndex {
public:
    explicit IdIndex(std::size_t max_entries) {
        std::size_t want = max_entries < 8 ? 16 : max_entries * 2;
        std::size_t cap = 16;
        while (cap < want) {
            cap *= 2;
        }
        table_.resize(cap);
        mask_ = cap - 1;
    }

    // Precondition: key != 0, key not present, size() < max_entries.
    void insert(OrderId key, std::uint32_t slot) noexcept {
        assert(key != 0);
        std::size_t i = home(key);
        while (table_[i].key != 0) {
            assert(table_[i].key != key);
            i = (i + 1) & mask_;
        }
        table_[i] = Cell{key, slot};
        ++size_;
    }

    // npos32 when absent. One probe run, usually one cache line.
    [[nodiscard]] std::uint32_t find(OrderId key) const noexcept {
        if (key == 0) {
            return npos32;
        }
        std::size_t i = home(key);
        while (table_[i].key != 0) {
            if (table_[i].key == key) {
                return table_[i].slot;
            }
            i = (i + 1) & mask_;
        }
        return npos32;
    }

    // Backward-shift deletion: close the hole by walking the probe run and
    // pulling back any entry whose home position permits it, so no tombstone
    // is ever left behind.
    bool erase(OrderId key) noexcept {
        if (key == 0) {
            return false;
        }
        std::size_t i = home(key);
        while (table_[i].key != key) {
            if (table_[i].key == 0) {
                return false;
            }
            i = (i + 1) & mask_;
        }
        std::size_t hole = i;
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (table_[j].key == 0) {
                break;
            }
            // The entry at j may move into the hole unless its home lies in
            // the cyclic interval (hole, j] — moving it then would put it
            // before its home and lookups would miss it.
            const std::size_t h = home(table_[j].key);
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                table_[hole] = table_[j];
                hole = j;
            }
        }
        table_[hole] = Cell{};
        --size_;
        return true;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    struct Cell {
        OrderId key{0};  // 0 = empty
        std::uint32_t slot{0};
    };

    // splitmix64 finalizer: cheap, and disperses sequential ids (the common
    // client pattern) across the table instead of clustering them.
    static std::uint64_t mix(std::uint64_t z) noexcept {
        z += 0x9e3779b97f4a7c15ull;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    [[nodiscard]] std::size_t home(OrderId key) const noexcept {
        return static_cast<std::size_t>(mix(key)) & mask_;
    }

    std::vector<Cell> table_;
    std::size_t mask_{0};
    std::size_t size_{0};
};

}  // namespace ob
