#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

#include "ob/types.hpp"

// Three-tier occupancy bitmap over price levels — the structure that makes
// best-price tracking O(1) without a map, a heap, or a scan.
//
// One bit per tick in the leaf tier (l0). One bit per leaf word in the middle
// tier (l1). One bit per middle word in the single top word (l2). With 64-bit
// words that covers up to 64^3 = 262,144 ticks, and any query — best bid,
// best ask, next occupied level in either direction — is at most three
// find-first-set instructions and three masked loads, all of which live in
// cache because the whole structure for a 65,536-tick band is ~8 KiB.
//
// Why not a min-max heap over active prices: a heap gives O(log P) for the
// operation this book does most (a cancel emptying a level = arbitrary
// deletion, which a heap cannot do in O(1) without an auxiliary index), costs
// a comparison-and-swap chain of unpredictable branches, and scatters its
// nodes' priorities across memory. The bitmap does the same job with three
// branch-free word operations on memory that never moves. See DECISIONS.md 003.

namespace ob {

class TieredBitmap {
public:
    static constexpr std::size_t max_size = std::size_t{64} * 64 * 64;

    explicit TieredBitmap(std::size_t n)
        : l0_((n + 63) / 64, 0), l1_((l0_.size() + 63) / 64, 0), n_(n) {
        assert(n >= 1 && n <= max_size);
    }

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    void set(std::uint32_t i) noexcept {
        assert(i < n_);
        const std::uint32_t w0 = i >> 6;
        const std::uint32_t w1 = i >> 12;
        l0_[w0] |= bit(i & 63u);
        l1_[w1] |= bit(w0 & 63u);
        l2_ |= bit(w1);
    }

    void clear(std::uint32_t i) noexcept {
        assert(i < n_);
        const std::uint32_t w0 = i >> 6;
        l0_[w0] &= ~bit(i & 63u);
        if (l0_[w0] == 0) {
            const std::uint32_t w1 = i >> 12;
            l1_[w1] &= ~bit(w0 & 63u);
            if (l1_[w1] == 0) {
                l2_ &= ~bit(w1);
            }
        }
    }

    [[nodiscard]] bool test(std::uint32_t i) const noexcept {
        assert(i < n_);
        return (l0_[i >> 6] & bit(i & 63u)) != 0;
    }

    [[nodiscard]] bool any() const noexcept { return l2_ != 0; }

    // Lowest set index, or npos32 if empty. This is "best ask".
    [[nodiscard]] std::uint32_t first() const noexcept {
        if (l2_ == 0) {
            return npos32;
        }
        const std::uint32_t w1 = lsb(l2_);
        const std::uint32_t w0 = (w1 << 6) | lsb(l1_[w1]);
        return (w0 << 6) | lsb(l0_[w0]);
    }

    // Highest set index, or npos32 if empty. This is "best bid".
    [[nodiscard]] std::uint32_t last() const noexcept {
        if (l2_ == 0) {
            return npos32;
        }
        const std::uint32_t w1 = msb(l2_);
        const std::uint32_t w0 = (w1 << 6) | msb(l1_[w1]);
        return (w0 << 6) | msb(l0_[w0]);
    }

    // Smallest set index strictly greater than i, or npos32. Walking the ask
    // side away from the touch (depth snapshots, FOK pre-pass).
    [[nodiscard]] std::uint32_t next_above(std::uint32_t i) const noexcept {
        assert(i < n_);
        if (i + 1 >= n_) {
            return npos32;
        }
        const std::uint32_t t = i + 1;
        const std::uint32_t w0 = t >> 6;
        if (const std::uint64_t m = l0_[w0] & (~std::uint64_t{0} << (t & 63u))) {
            return (w0 << 6) | lsb(m);
        }
        const std::uint32_t w1 = w0 >> 6;
        const std::uint32_t b1 = (w0 & 63u) + 1;
        if (b1 < 64) {
            if (const std::uint64_t m = l1_[w1] & (~std::uint64_t{0} << b1)) {
                const std::uint32_t v0 = (w1 << 6) | lsb(m);
                return (v0 << 6) | lsb(l0_[v0]);
            }
        }
        const std::uint32_t b2 = w1 + 1;
        if (b2 >= 64) {
            return npos32;
        }
        const std::uint64_t m = l2_ & (~std::uint64_t{0} << b2);
        if (m == 0) {
            return npos32;
        }
        const std::uint32_t v1 = lsb(m);
        const std::uint32_t v0 = (v1 << 6) | lsb(l1_[v1]);
        return (v0 << 6) | lsb(l0_[v0]);
    }

    // Largest set index strictly less than i, or npos32. Walking the bid side
    // away from the touch.
    [[nodiscard]] std::uint32_t next_below(std::uint32_t i) const noexcept {
        assert(i < n_);
        if (i == 0) {
            return npos32;
        }
        const std::uint32_t t = i - 1;
        const std::uint32_t w0 = t >> 6;
        if (const std::uint64_t m =
                l0_[w0] & (~std::uint64_t{0} >> (63u - (t & 63u)))) {
            return (w0 << 6) | msb(m);
        }
        const std::uint32_t w1 = w0 >> 6;
        const std::uint32_t b1 = w0 & 63u;
        if (b1 > 0) {
            if (const std::uint64_t m =
                    l1_[w1] & (~std::uint64_t{0} >> (64u - b1))) {
                const std::uint32_t v0 = (w1 << 6) | msb(m);
                return (v0 << 6) | msb(l0_[v0]);
            }
        }
        if (w1 == 0) {
            return npos32;
        }
        const std::uint64_t m = l2_ & (~std::uint64_t{0} >> (64u - w1));
        if (m == 0) {
            return npos32;
        }
        const std::uint32_t v1 = msb(m);
        const std::uint32_t v0 = (v1 << 6) | msb(l1_[v1]);
        return (v0 << 6) | msb(l0_[v0]);
    }

private:
    static constexpr std::uint64_t bit(std::uint32_t b) noexcept {
        return std::uint64_t{1} << b;
    }
    static std::uint32_t lsb(std::uint64_t w) noexcept {
        assert(w != 0);
        return static_cast<std::uint32_t>(std::countr_zero(w));
    }
    static std::uint32_t msb(std::uint64_t w) noexcept {
        assert(w != 0);
        return 63u - static_cast<std::uint32_t>(std::countl_zero(w));
    }

    std::vector<std::uint64_t> l0_;  // one bit per tick
    std::vector<std::uint64_t> l1_;  // one bit per l0 word
    std::uint64_t l2_{0};            // one bit per l1 word
    std::size_t n_;
};

}  // namespace ob
