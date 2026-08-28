#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <list>
#include <optional>
#include <span>
#include <vector>

#include "ob/events.hpp"
#include "ob/instrument.hpp"
#include "ob/order.hpp"
#include "ob/types.hpp"

// The reference engine — the correctness oracle and performance baseline
// (DECISIONS.md 002). Deliberately obvious and deliberately slow; it is never
// deleted, because it is what licenses every claim made about FastBook.
//
// Deliberately obvious does not require std::map (DECISIONS.md 010): each
// side is a std::vector of levels kept sorted by price, searched and spliced
// with the standard algorithms, and orders live in a std::list per level.
// Cancel and lookup are linear scans of the whole book — transparency over
// speed is the entire job description here. There is no id index at all: the
// scan *is* the specification of what an id lookup must return.
//
// Event-for-event semantics are the contract shared with FastBook
// (ARCHITECTURE.md 4-5); the differential test layer holds both engines to it.

namespace ob {

template <EventSink Sink = NullSink>
class NaiveBook {
public:
    NaiveBook(Price base_price, std::size_t num_ticks, std::size_t capacity)
        : NaiveBook(OptionSeries{}, base_price, num_ticks, capacity) {}

    NaiveBook(const OptionSeries& series, Price base_price,
              std::size_t num_ticks, std::size_t capacity)
        : base_(base_price),
          num_ticks_(num_ticks),
          capacity_(capacity),
          series_(series) {}

    NaiveBook(const NaiveBook&) = delete;
    NaiveBook& operator=(const NaiveBook&) = delete;

    // ---- commands ---------------------------------------------------------

    void submit(OrderId id, Side side, OrderType type, Price price,
                Quantity qty) {
        if (id == 0) {
            sink_(Event::rejected(id, side, RejectReason::InvalidId, price, qty));
            return;
        }
        if (locate(id).found) {
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
        if (type == OrderType::Limit && live_ == capacity_) {
            sink_(Event::rejected(id, side, RejectReason::CapacityExhausted, price, qty));
            return;
        }

        const Price limit =
            type == OrderType::Market
                ? (side == Side::Buy
                       ? base_ + static_cast<Price>(num_ticks_) - 1
                       : base_)
                : price;

        if (type == OrderType::FOK && crossable_quantity(side, limit, qty) < qty) {
            sink_(Event::rejected(id, side, RejectReason::FokUnfillable, price, qty));
            return;
        }

        const Quantity remaining = match(id, side, limit, qty);
        if (remaining > 0 && type == OrderType::Limit) {
            rest(id, side, price, remaining);
        }
    }

    void cancel(OrderId id) {
        const Loc loc = locate(id);
        if (!loc.found) {
            sink_(Event::rejected(id, Side::Buy, RejectReason::UnknownOrder, 0, 0));
            return;
        }
        sink_(Event::cancelled(id, loc.it->side, loc.it->price, loc.it->qty));
        remove_at(loc);
    }

    void modify(OrderId id, Price new_price, Quantity new_qty) {
        const Loc loc = locate(id);
        if (!loc.found) {
            sink_(Event::rejected(id, Side::Buy, RejectReason::UnknownOrder,
                                  new_price, new_qty));
            return;
        }
        if (new_qty == 0) {
            sink_(Event::cancelled(id, loc.it->side, loc.it->price, loc.it->qty));
            remove_at(loc);
            return;
        }
        if (new_price == loc.it->price && new_qty <= loc.it->qty) {
            const Quantity delta = loc.it->qty - new_qty;
            loc.it->qty = new_qty;
            side_of(loc.side)[loc.level_ix].agg -= delta;
            sink_(Event::reduced(id, loc.side, loc.it->price, new_qty));
            return;
        }
        const Side side = loc.it->side;
        sink_(Event::cancelled(id, side, loc.it->price, loc.it->qty));
        remove_at(loc);
        submit(id, side, OrderType::Limit, new_price, new_qty);
    }

    // ---- queries ----------------------------------------------------------

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        const auto& bids = side_of(Side::Buy);
        if (bids.empty()) {
            return std::nullopt;
        }
        return bids.back().price;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        const auto& asks = side_of(Side::Sell);
        if (asks.empty()) {
            return std::nullopt;
        }
        return asks.front().price;
    }

    [[nodiscard]] Quantity quantity_at(Side side, Price price) const noexcept {
        const auto& levels = side_of(side);
        const auto it = find_level(levels, price);
        return it != levels.end() && it->price == price ? it->agg : 0;
    }

    [[nodiscard]] std::optional<Quantity> open_quantity(OrderId id) const {
        const Loc loc = locate(id);
        if (!loc.found) {
            return std::nullopt;
        }
        return loc.it->qty;
    }

    [[nodiscard]] std::size_t order_count() const noexcept { return live_; }

    std::size_t snapshot(Side side, std::span<LevelView> out) const noexcept {
        const auto& levels = side_of(side);
        std::size_t n = 0;
        if (side == Side::Buy) {
            for (auto it = levels.rbegin(); it != levels.rend() && n < out.size();
                 ++it) {
                out[n++] = LevelView{it->price, it->agg,
                                     static_cast<std::uint32_t>(it->fifo.size())};
            }
        } else {
            for (auto it = levels.begin(); it != levels.end() && n < out.size();
                 ++it) {
                out[n++] = LevelView{it->price, it->agg,
                                     static_cast<std::uint32_t>(it->fifo.size())};
            }
        }
        return n;
    }

    [[nodiscard]] const OptionSeries& instrument() const noexcept { return series_; }
    [[nodiscard]] Price base_price() const noexcept { return base_; }
    [[nodiscard]] std::size_t num_ticks() const noexcept { return num_ticks_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] Sink& sink() noexcept { return sink_; }
    [[nodiscard]] const Sink& sink() const noexcept { return sink_; }

    [[nodiscard]] const char* validate() const {
        const auto bb = best_bid();
        const auto ba = best_ask();
        if (bb && ba && *bb >= *ba) {
            return "book is crossed";
        }
        std::size_t seen = 0;
        for (const Side side : {Side::Buy, Side::Sell}) {
            const auto& levels = side_of(side);
            Price prev_price = 0;
            bool have_prev = false;
            for (const auto& level : levels) {
                if (have_prev && level.price <= prev_price) {
                    return "levels are not strictly ascending";
                }
                prev_price = level.price;
                have_prev = true;
                if (level.fifo.empty()) {
                    return "active level is empty";
                }
                Quantity sum = 0;
                for (const Order& o : level.fifo) {
                    if (o.side != side || o.price != level.price) {
                        return "order filed under the wrong level";
                    }
                    if (o.qty == 0) {
                        return "resting order has zero quantity";
                    }
                    sum += o.qty;
                }
                if (sum != level.agg) {
                    return "cached level aggregate has drifted";
                }
                seen += level.fifo.size();
            }
        }
        if (seen != live_) {
            return "live order count does not match resting orders";
        }
        return nullptr;
    }

private:
    struct NaiveLevel {
        Price price{0};
        Quantity agg{0};
        std::list<Order> fifo;
    };
    using Levels = std::vector<NaiveLevel>;

    struct Loc {
        bool found{false};
        Side side{Side::Buy};
        std::size_t level_ix{0};
        std::list<Order>::iterator it{};
    };

    [[nodiscard]] bool in_band(Price p) const noexcept {
        return p >= base_ && p < base_ + static_cast<Price>(num_ticks_);
    }
    [[nodiscard]] Levels& side_of(Side s) noexcept {
        return sides_[static_cast<std::size_t>(s)];
    }
    [[nodiscard]] const Levels& side_of(Side s) const noexcept {
        return sides_[static_cast<std::size_t>(s)];
    }

    static typename Levels::const_iterator find_level(const Levels& levels,
                                                      Price price) noexcept {
        return std::lower_bound(
            levels.begin(), levels.end(), price,
            [](const NaiveLevel& l, Price p) { return l.price < p; });
    }
    static typename Levels::iterator find_level(Levels& levels,
                                                Price price) noexcept {
        return std::lower_bound(
            levels.begin(), levels.end(), price,
            [](const NaiveLevel& l, Price p) { return l.price < p; });
    }

    // The linear scan that defines what "id lookup" means. O(live orders).
    [[nodiscard]] Loc locate(OrderId id) const {
        auto* self = const_cast<NaiveBook*>(this);
        for (const Side side : {Side::Buy, Side::Sell}) {
            Levels& levels = self->side_of(side);
            for (std::size_t li = 0; li < levels.size(); ++li) {
                for (auto it = levels[li].fifo.begin();
                     it != levels[li].fifo.end(); ++it) {
                    if (it->id == id) {
                        return Loc{true, side, li, it};
                    }
                }
            }
        }
        return Loc{};
    }

    [[nodiscard]] Quantity crossable_quantity(Side side, Price limit,
                                              Quantity need) const noexcept {
        const Levels& levels = side_of(opposite(side));
        Quantity avail = 0;
        if (side == Side::Buy) {
            for (const auto& level : levels) {
                if (!is_better_or_equal(side, limit, level.price)) {
                    break;
                }
                avail += level.agg;
                if (avail >= need) {
                    break;
                }
            }
        } else {
            for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
                if (!is_better_or_equal(side, limit, it->price)) {
                    break;
                }
                avail += it->agg;
                if (avail >= need) {
                    break;
                }
            }
        }
        return avail;
    }

    Quantity match(OrderId taker, Side side, Price limit, Quantity qty) {
        Levels& levels = side_of(opposite(side));
        Quantity remaining = qty;
        while (remaining > 0 && !levels.empty()) {
            // Best opposite level: lowest ask for a buyer, highest bid for a
            // seller.
            const std::size_t ix = side == Side::Buy ? 0 : levels.size() - 1;
            NaiveLevel& level = levels[ix];
            if (!is_better_or_equal(side, limit, level.price)) {
                break;
            }
            const Price level_price = level.price;
            while (remaining > 0 && !level.fifo.empty()) {
                Order& maker = level.fifo.front();
                const Quantity fill = remaining < maker.qty ? remaining : maker.qty;
                maker.qty -= fill;
                level.agg -= fill;
                remaining -= fill;
                sink_(Event::trade(taker, maker.id, side, level_price, fill));
                if (maker.qty == 0) {
                    level.fifo.pop_front();
                    --live_;
                }
            }
            if (level.fifo.empty()) {
                levels.erase(levels.begin() + static_cast<std::ptrdiff_t>(ix));
            }
        }
        return remaining;
    }

    void rest(OrderId id, Side side, Price price, Quantity qty) {
        Levels& levels = side_of(side);
        auto it = find_level(levels, price);
        if (it == levels.end() || it->price != price) {
            it = levels.insert(it, NaiveLevel{price, 0, {}});
        }
        it->fifo.push_back(Order{id, side, OrderType::Limit, price, qty});
        it->agg += qty;
        ++live_;
        sink_(Event::accepted(id, side, price, qty));
    }

    void remove_at(const Loc& loc) {
        Levels& levels = side_of(loc.side);
        NaiveLevel& level = levels[loc.level_ix];
        level.agg -= loc.it->qty;
        level.fifo.erase(loc.it);
        --live_;
        if (level.fifo.empty()) {
            levels.erase(levels.begin() +
                         static_cast<std::ptrdiff_t>(loc.level_ix));
        }
    }

    Price base_;
    std::size_t num_ticks_;
    std::size_t capacity_;
    OptionSeries series_;
    std::array<Levels, 2> sides_;  // [Buy, Sell], each sorted ascending
    std::size_t live_{0};
    [[no_unique_address]] Sink sink_{};
};

}  // namespace ob
