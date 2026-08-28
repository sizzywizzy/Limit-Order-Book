#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "ob/bitmap.hpp"
#include "ob/events.hpp"
#include "ob/id_index.hpp"
#include "ob/instrument.hpp"
#include "ob/intrusive_list.hpp"
#include "ob/pool.hpp"
#include "ob/types.hpp"

// The optimised engine.
//
//   Price levels    flat array indexed by tick offset from base — O(1), no map
//   Best bid/ask    three-tier occupancy bitmap — O(1), no heap, no scan
//   FIFO per level  intrusive doubly-linked list of pool slots — O(1) unlink
//   Order storage   pre-allocated SlotPool — zero allocation after construction
//   Id lookup       flat open-addressing IdIndex — O(1), no unordered_map
//
// Zero heap allocation on every operation after construction; the property
// test layer proves this with a counting allocator rather than asserting it.
//
// Cancel — the operation that dominates real flow and therefore the one this
// engine is shaped around — is: one hash probe, four link writes, one
// aggregate decrement, one conditional bitmap clear, one free-list push.
// There is no loop and no search anywhere in that path.
//
// Semantics are defined by ARCHITECTURE.md 4 and are identical, event for
// event, to NaiveBook — enforced by the differential test layer.

namespace ob {

// Pool slot: an order plus its intrusive links, in exactly 32 bytes — two to
// a 64-byte cache line, and a FIFO walk never straddles a line for a single
// order.
//
// There is deliberately no `price` field: a resting order's price is
// base + tick by construction, so storing it too would be eight bytes of
// duplicated state that could drift from `tick` and would push the slot to 40
// bytes (1.6 per line). `price_of()` recomputes it in one add. See RESULTS.md,
// optimisation history #1.
struct FastOrder {
    OrderId id{0};
    Quantity qty{0};              // remaining
    std::uint32_t prev{npos32};   // intrusive FIFO links (slot indices);
    std::uint32_t next{npos32};   // `next` doubles as the pool free list
    std::uint32_t tick{0};        // level index — the price, as an offset
    Side side{Side::Buy};
    std::uint8_t pad_[3]{};
};

// The packing is a load-bearing claim, so it is checked rather than commented.
static_assert(sizeof(FastOrder) == 32, "FastOrder must stay two per cache line");

template <EventSink Sink = NullSink>
class FastBook {
public:
    // Covers prices in [base_price, base_price + num_ticks). Orders outside
    // are rejected explicitly (DECISIONS.md 003). Holds at most `capacity`
    // resting orders (DECISIONS.md 005).
    FastBook(Price base_price, std::size_t num_ticks, std::size_t capacity)
        : FastBook(OptionSeries{}, base_price, num_ticks, capacity) {}

    FastBook(const OptionSeries& series, Price base_price,
             std::size_t num_ticks, std::size_t capacity)
        : base_(base_price),
          num_ticks_(num_ticks),
          series_(series),
          pool_(capacity),
          index_(capacity),
          levels_{std::vector<Level>(num_ticks), std::vector<Level>(num_ticks)},
          bits_{TieredBitmap(num_ticks), TieredBitmap(num_ticks)} {
        assert(num_ticks >= 1 && num_ticks <= TieredBitmap::max_size);
    }

    FastBook(const FastBook&) = delete;
    FastBook& operator=(const FastBook&) = delete;

    // ---- commands ---------------------------------------------------------

    void submit(OrderId id, Side side, OrderType type, Price price,
                Quantity qty) {
        if (id == 0) {
            sink_(Event::rejected(id, side, RejectReason::InvalidId, price, qty));
            return;
        }
        if (index_.find(id) != npos32) {
            sink_(Event::rejected(id, side, RejectReason::DuplicateId, price, qty));
            return;
        }
        if (qty == 0) {
            sink_(Event::rejected(id, side, RejectReason::ZeroQuantity, price, qty));
            return;
        }
        if (type != OrderType::Market && !in_band(price)) {
            sink_(Event::rejected(id, side, RejectReason::PriceOutOfRange, price, qty));
            return;
        }
        if (type == OrderType::Limit && pool_.in_use() == pool_.capacity()) {
            sink_(Event::rejected(id, side, RejectReason::CapacityExhausted, price, qty));
            return;
        }

        // Market orders have no limit: match to the edge of the band. They
        // can never rest, so the band edge is only a bound on the sweep.
        const Price limit =
            type == OrderType::Market
                ? (side == Side::Buy
                       ? base_ + static_cast<Price>(num_ticks_) - 1
                       : base_)
                : price;

        // FOK must not partially execute, so availability is established
        // before the first fill (ARCHITECTURE.md 4.1).
        if (type == OrderType::FOK && crossable_quantity(side, limit, qty) < qty) {
            sink_(Event::rejected(id, side, RejectReason::FokUnfillable, price, qty));
            return;
        }

        const Quantity remaining = match(id, side, limit, qty);
        if (remaining > 0 && type == OrderType::Limit) {
            rest(id, side, price, remaining);
        }
        // Market and IOC residuals are discarded; FOK has none by pre-check.
    }

    void cancel(OrderId id) {
        const std::uint32_t i = index_.find(id);
        if (i == npos32) {
            sink_(Event::rejected(id, Side::Buy, RejectReason::UnknownOrder, 0, 0));
            return;
        }
        const FastOrder& o = pool_[i];
        sink_(Event::cancelled(id, o.side, price_of(o), o.qty));
        remove_resting(i);
    }

    // Quantity reduction at the same price updates in place and keeps queue
    // position; a price change or quantity increase is a cancel-replace and
    // loses it (ARCHITECTURE.md 4.3, DECISIONS.md 006).
    void modify(OrderId id, Price new_price, Quantity new_qty) {
        const std::uint32_t i = index_.find(id);
        if (i == npos32) {
            sink_(Event::rejected(id, Side::Buy, RejectReason::UnknownOrder,
                                  new_price, new_qty));
            return;
        }
        FastOrder& o = pool_[i];
        const Price resting_price = price_of(o);
        if (new_qty == 0) {
            sink_(Event::cancelled(id, o.side, resting_price, o.qty));
            remove_resting(i);
            return;
        }
        if (new_price == resting_price && new_qty <= o.qty) {
            const Quantity delta = o.qty - new_qty;
            o.qty = new_qty;
            levels_of(o.side)[o.tick].agg -= delta;
            sink_(Event::reduced(id, o.side, resting_price, new_qty));
            return;
        }
        const Side side = o.side;
        sink_(Event::cancelled(id, side, resting_price, o.qty));
        remove_resting(i);
        submit(id, side, OrderType::Limit, new_price, new_qty);
    }

    // ---- queries ----------------------------------------------------------

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        const std::uint32_t t = bits_of(Side::Buy).last();
        if (t == npos32) {
            return std::nullopt;
        }
        return base_ + static_cast<Price>(t);
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        const std::uint32_t t = bits_of(Side::Sell).first();
        if (t == npos32) {
            return std::nullopt;
        }
        return base_ + static_cast<Price>(t);
    }

    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept {
        if (!in_band(price)) {
            return 0;
        }
        return levels_of(side)[tick_of(price)].agg;
    }

    [[nodiscard]] std::optional<Quantity> open_quantity(OrderId id) const noexcept {
        const std::uint32_t i = index_.find(id);
        if (i == npos32) {
            return std::nullopt;
        }
        return pool_[i].qty;
    }

    [[nodiscard]] std::size_t order_count() const noexcept {
        return pool_.in_use();
    }

    // Level-2 depth, best price first. Fills at most out.size() rows and
    // returns the number written.
    std::size_t snapshot(Side side, std::span<LevelView> out) const noexcept {
        const TieredBitmap& b = bits_of(side);
        const std::vector<Level>& lv = levels_of(side);
        std::size_t n = 0;
        std::uint32_t t = side == Side::Buy ? b.last() : b.first();
        while (t != npos32 && n < out.size()) {
            out[n++] = LevelView{base_ + static_cast<Price>(t), lv[t].agg,
                                 lv[t].count};
            t = side == Side::Buy ? b.next_below(t) : b.next_above(t);
        }
        return n;
    }

    [[nodiscard]] const OptionSeries& instrument() const noexcept { return series_; }
    [[nodiscard]] Price base_price() const noexcept { return base_; }
    [[nodiscard]] std::size_t num_ticks() const noexcept { return num_ticks_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return pool_.capacity(); }

    [[nodiscard]] Sink& sink() noexcept { return sink_; }
    [[nodiscard]] const Sink& sink() const noexcept { return sink_; }

    // Structural invariant check for the property test layer: walks only the
    // occupied levels, so cost is O(resting orders), not O(num_ticks).
    // Returns nullptr when every invariant holds, else a description.
    [[nodiscard]] const char* validate() const {
        const auto bb = best_bid();
        const auto ba = best_ask();
        if (bb && ba && *bb >= *ba) {
            return "book is crossed";
        }
        std::size_t seen = 0;
        for (const Side side : {Side::Buy, Side::Sell}) {
            const TieredBitmap& b = bits_of(side);
            const std::vector<Level>& lv = levels_of(side);
            std::uint32_t t = b.first();
            while (t != npos32) {
                const Level& level = lv[t];
                if (level.count == 0 || level.fifo.empty()) {
                    return "active level is empty";
                }
                Quantity sum = 0;
                std::uint32_t n = 0;
                std::uint32_t prev = npos32;
                std::uint32_t i = level.fifo.head;
                while (i != npos32) {
                    const FastOrder& o = pool_[i];
                    if (o.prev != prev) {
                        return "FIFO back-link mismatch";
                    }
                    if (o.side != side || o.tick != t) {
                        return "order filed under the wrong level";
                    }
                    if (o.qty == 0) {
                        return "resting order has zero quantity";
                    }
                    if (index_.find(o.id) != i) {
                        return "id index does not resolve to the resting order";
                    }
                    sum += o.qty;
                    ++n;
                    prev = i;
                    i = o.next;
                }
                if (level.fifo.tail != prev) {
                    return "FIFO tail does not match last node";
                }
                if (sum != level.agg) {
                    return "cached level aggregate has drifted";
                }
                if (n != level.count) {
                    return "cached level order count has drifted";
                }
                seen += n;
                t = b.next_above(t);
            }
        }
        // Counts tie the three structures together: if a non-empty level
        // lost its occupancy bit, its orders are missing from `seen` and
        // this is where that shows up.
        if (seen != pool_.in_use()) {
            return "pool in-use count does not match resting orders";
        }
        if (seen != index_.size()) {
            return "id index size does not match resting orders";
        }
        return nullptr;
    }

private:
    struct Level {
        IndexFifo fifo;
        Quantity agg{0};
        std::uint32_t count{0};
    };

    [[nodiscard]] bool in_band(Price p) const noexcept {
        return p >= base_ && p < base_ + static_cast<Price>(num_ticks_);
    }
    [[nodiscard]] std::uint32_t tick_of(Price p) const noexcept {
        assert(in_band(p));
        return static_cast<std::uint32_t>(p - base_);
    }
    // A resting order's price, recomputed rather than stored (see FastOrder).
    [[nodiscard]] Price price_of(const FastOrder& o) const noexcept {
        return base_ + static_cast<Price>(o.tick);
    }
    [[nodiscard]] static std::size_t side_ix(Side s) noexcept {
        return static_cast<std::size_t>(s);
    }
    [[nodiscard]] std::vector<Level>& levels_of(Side s) noexcept {
        return levels_[side_ix(s)];
    }
    [[nodiscard]] const std::vector<Level>& levels_of(Side s) const noexcept {
        return levels_[side_ix(s)];
    }
    [[nodiscard]] TieredBitmap& bits_of(Side s) noexcept {
        return bits_[side_ix(s)];
    }
    [[nodiscard]] const TieredBitmap& bits_of(Side s) const noexcept {
        return bits_[side_ix(s)];
    }

    // Total opposite-side quantity at prices crossing `limit`, stopping as
    // soon as `need` is covered. FOK pre-pass only.
    [[nodiscard]] Quantity crossable_quantity(Side side, Price limit,
                                              Quantity need) const noexcept {
        const TieredBitmap& b = bits_of(opposite(side));
        const std::vector<Level>& lv = levels_of(opposite(side));
        Quantity avail = 0;
        std::uint32_t t = side == Side::Buy ? b.first() : b.last();
        while (t != npos32) {
            if (!is_better_or_equal(side, limit, base_ + static_cast<Price>(t))) {
                break;
            }
            avail += lv[t].agg;
            if (avail >= need) {
                break;
            }
            t = side == Side::Buy ? b.next_above(t) : b.next_below(t);
        }
        return avail;
    }

    // Price-time priority sweep: best opposite level first, FIFO order within
    // a level. Returns the taker's unfilled remainder.
    Quantity match(OrderId taker, Side side, Price limit, Quantity qty) {
        TieredBitmap& b = bits_of(opposite(side));
        std::vector<Level>& lv = levels_of(opposite(side));
        Quantity remaining = qty;
        while (remaining > 0) {
            const std::uint32_t t = side == Side::Buy ? b.first() : b.last();
            if (t == npos32) {
                break;
            }
            const Price level_price = base_ + static_cast<Price>(t);
            if (!is_better_or_equal(side, limit, level_price)) {
                break;
            }
            Level& level = lv[t];
            while (remaining > 0 && !level.fifo.empty()) {
                const std::uint32_t mi = level.fifo.head;
                FastOrder& maker = pool_[mi];
                const Quantity fill = remaining < maker.qty ? remaining : maker.qty;
                maker.qty -= fill;
                level.agg -= fill;
                remaining -= fill;
                sink_(Event::trade(taker, maker.id, side, level_price, fill));
                if (maker.qty == 0) {
                    level.fifo.unlink(pool_, mi);
                    --level.count;
                    // Erase from the index *before* releasing the slot —
                    // the use-after-free hazard of ARCHITECTURE.md 3.5.
                    index_.erase(maker.id);
                    pool_.release(mi);
                }
            }
            if (level.count == 0) {
                b.clear(t);
            }
        }
        return remaining;
    }

    void rest(OrderId id, Side side, Price price, Quantity qty) {
        const std::uint32_t i = pool_.acquire();
        // Capacity was checked before matching, and matching only releases
        // slots, so acquisition cannot fail here.
        assert(i != npos32);
        FastOrder& o = pool_[i];
        o.id = id;
        o.qty = qty;
        o.tick = tick_of(price);
        o.side = side;
        Level& level = levels_of(side)[o.tick];
        level.fifo.push_back(pool_, i);
        ++level.count;
        level.agg += qty;
        if (level.count == 1) {
            bits_of(side).set(o.tick);
        }
        index_.insert(id, i);
        sink_(Event::accepted(id, side, price, qty));
    }

    // Shared tail of cancel / modify: unlink, un-index, release. No search.
    void remove_resting(std::uint32_t i) {
        FastOrder& o = pool_[i];
        Level& level = levels_of(o.side)[o.tick];
        level.fifo.unlink(pool_, i);
        --level.count;
        level.agg -= o.qty;
        if (level.count == 0) {
            bits_of(o.side).clear(o.tick);
        }
        index_.erase(o.id);  // before release — ARCHITECTURE.md 3.5
        pool_.release(i);
    }

    Price base_;
    std::size_t num_ticks_;
    OptionSeries series_;
    SlotPool<FastOrder> pool_;
    IdIndex index_;
    std::array<std::vector<Level>, 2> levels_;  // [Buy, Sell]
    std::array<TieredBitmap, 2> bits_;          // [Buy, Sell]
    [[no_unique_address]] Sink sink_{};
};

}  // namespace ob
