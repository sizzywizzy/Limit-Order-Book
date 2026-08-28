#pragma once

// Shared test machinery for the property, differential and standalone layers.
//
// Everything here is deliberately engine-agnostic: the flow generator emits a
// deterministic operation sequence from a seed with no feedback from any book,
// so the same sequence can be applied to both engines; the ledger rebuilds
// expected state purely from the emitted event stream and then reconciles it
// against the book's queries. A test failure therefore names the seed and the
// operation index, which is what makes it replayable.
//
// The generator uses only its own arithmetic on std::mt19937_64 raw output —
// no standard <random> distributions, whose algorithms are implementation-
// defined — so a recorded seed reproduces the identical sequence on every
// compiler and standard library.

#include <cstddef>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ob/book_fast.hpp"
#include "ob/book_naive.hpp"
#include "ob/events.hpp"
#include "ob/types.hpp"

namespace obtest {

struct BookParams {
    ob::Price base{1000};
    std::size_t num_ticks{512};
    std::size_t capacity{1024};
};

struct Op {
    enum class Kind : std::uint8_t { Submit, Cancel, Modify };
    Kind kind{Kind::Submit};
    ob::OrderId id{0};
    ob::Side side{ob::Side::Buy};
    ob::OrderType type{ob::OrderType::Limit};
    ob::Price price{0};    // submit: limit price; modify: new price
    ob::Quantity qty{0};   // submit: quantity;   modify: new quantity
};

inline std::uint64_t rnd(std::mt19937_64& g, std::uint64_t n) {
    return g() % n;  // modulo bias is irrelevant for test flow
}

// Trials until a `stop_percent` coin lands, capped. Small values common,
// long tail rare — used for price offsets and recency bias.
inline std::uint64_t geometric(std::mt19937_64& g, std::uint64_t stop_percent,
                               std::uint64_t cap) {
    std::uint64_t k = 0;
    while (k < cap && rnd(g, 100) >= stop_percent) {
        ++k;
    }
    return k;
}

class FlowGen {
public:
    FlowGen(std::uint64_t seed, const BookParams& p) : rng_(seed), p_(p) {}

    Op next() {
        const std::uint64_t roll = rnd(rng_, 100);
        if (issued_.empty() || roll < 45) {
            return gen_submit();
        }
        if (roll < 80) {
            return gen_cancel();
        }
        return gen_modify();
    }

private:
    Op gen_submit() {
        Op op;
        op.kind = Op::Kind::Submit;
        op.id = next_id_++;
        op.side = rnd(rng_, 2) == 0 ? ob::Side::Buy : ob::Side::Sell;
        const std::uint64_t t = rnd(rng_, 100);
        op.type = t < 70   ? ob::OrderType::Limit
                  : t < 80 ? ob::OrderType::IOC
                  : t < 90 ? ob::OrderType::Market
                           : ob::OrderType::FOK;

        if (op.type == ob::OrderType::Market) {
            op.price = 0;  // convention: market orders carry no price
        } else if (rnd(rng_, 100) < 2) {
            // Deliberately out of band, to exercise PriceOutOfRange.
            op.price = rnd(rng_, 2) == 0
                           ? p_.base - 1 - static_cast<ob::Price>(rnd(rng_, 50))
                           : p_.base + static_cast<ob::Price>(p_.num_ticks +
                                                              rnd(rng_, 50));
        } else {
            // Concentrated near the middle of the band, thinning outward,
            // with buys biased below and sells above so a spread forms —
            // but overlapping enough that crossings are routine.
            const ob::Price mid =
                p_.base + static_cast<ob::Price>(p_.num_ticks / 2);
            const auto off = static_cast<ob::Price>(
                geometric(rng_, 18, p_.num_ticks / 2 - 1));
            const auto jitter = static_cast<ob::Price>(rnd(rng_, 7)) - 3;
            ob::Price px = op.side == ob::Side::Buy ? mid - off + jitter
                                                    : mid + off + jitter;
            const ob::Price lo = p_.base;
            const ob::Price hi = p_.base + static_cast<ob::Price>(p_.num_ticks) - 1;
            px = px < lo ? lo : (px > hi ? hi : px);
            op.price = px;
        }

        if (rnd(rng_, 100) < 1) {
            op.qty = 0;  // exercise ZeroQuantity
        } else if (rnd(rng_, 100) < 5) {
            op.qty = 100 + rnd(rng_, 1900);  // occasional sweeper
        } else {
            op.qty = 1 + rnd(rng_, 100);
        }

        issued_.emplace_back(op.id, op.price);
        return op;
    }

    Op gen_cancel() {
        Op op;
        op.kind = Op::Kind::Cancel;
        const std::uint64_t roll = rnd(rng_, 100);
        if (roll < 90) {
            op.id = pick_issued().first;
        } else if (roll < 98) {
            op.id = 1'000'000'000'000ull + rnd(rng_, 1'000'000);  // unknown
        } else {
            op.id = 0;  // reserved id
        }
        return op;
    }

    Op gen_modify() {
        Op op;
        op.kind = Op::Kind::Modify;
        const auto [id, submit_price] = pick_issued();
        op.id = id;
        if (rnd(rng_, 100) < 2) {
            op.qty = 0;  // modify-to-zero is a cancel
            op.price = submit_price;
            return op;
        }
        if (rnd(rng_, 100) < 60) {
            op.price = submit_price;  // same price: reduce path when qty falls
            op.qty = 1 + rnd(rng_, 80);
        } else {
            const auto shift = static_cast<ob::Price>(rnd(rng_, 11)) - 5;
            ob::Price px = submit_price + shift;
            const ob::Price lo = p_.base;
            const ob::Price hi = p_.base + static_cast<ob::Price>(p_.num_ticks) - 1;
            px = px < lo ? lo : (px > hi ? hi : px);
            op.price = px;
            op.qty = 1 + rnd(rng_, 120);
        }
        return op;
    }

    // Recency-biased: recent ids are more likely to still be resting, so most
    // cancels hit live orders while stale picks exercise UnknownOrder.
    std::pair<ob::OrderId, ob::Price> pick_issued() {
        const std::uint64_t back =
            geometric(rng_, 3, issued_.size() - 1);
        return issued_[issued_.size() - 1 - back];
    }

    std::mt19937_64 rng_;
    BookParams p_;
    ob::OrderId next_id_{1};
    std::vector<std::pair<ob::OrderId, ob::Price>> issued_;
};

template <typename Book>
void apply(Book& book, const Op& op) {
    switch (op.kind) {
        case Op::Kind::Submit:
            book.submit(op.id, op.side, op.type, op.price, op.qty);
            break;
        case Op::Kind::Cancel:
            book.cancel(op.id);
            break;
        case Op::Kind::Modify:
            book.modify(op.id, op.price, op.qty);
            break;
    }
}

inline const char* name_of(ob::EventType t) {
    switch (t) {
        case ob::EventType::Accepted: return "Accepted";
        case ob::EventType::Trade: return "Trade";
        case ob::EventType::Reduced: return "Reduced";
        case ob::EventType::Cancelled: return "Cancelled";
        case ob::EventType::Rejected: return "Rejected";
    }
    return "?";
}

inline std::string to_string(const ob::Event& e) {
    std::ostringstream os;
    os << name_of(e.type) << "{id=" << e.id << " maker=" << e.maker_id
       << " side=" << (e.side == ob::Side::Buy ? "Buy" : "Sell")
       << " px=" << e.price << " qty=" << e.qty
       << " reason=" << static_cast<int>(e.reason) << "}";
    return os.str();
}

inline std::string to_string(const Op& op) {
    std::ostringstream os;
    switch (op.kind) {
        case Op::Kind::Submit:
            os << "Submit{id=" << op.id
               << " side=" << (op.side == ob::Side::Buy ? "Buy" : "Sell")
               << " type=" << static_cast<int>(op.type) << " px=" << op.price
               << " qty=" << op.qty << "}";
            break;
        case Op::Kind::Cancel:
            os << "Cancel{id=" << op.id << "}";
            break;
        case Op::Kind::Modify:
            os << "Modify{id=" << op.id << " px=" << op.price
               << " qty=" << op.qty << "}";
            break;
    }
    return os.str();
}

// Rebuilds the resting-order set purely from the event stream, checking each
// event's legality as it goes, then reconciles against the book's own
// queries. This is the conservation invariant made executable: everything
// submitted is accounted for as traded, cancelled, resting, or discarded.
class Ledger {
public:
    // Returns "" or a description of the first illegal event.
    std::string consume(const std::vector<ob::Event>& events) {
        for (const ob::Event& e : events) {
            switch (e.type) {
                case ob::EventType::Accepted: {
                    if (resting_.contains(e.id)) {
                        return "Accepted for an id already resting: " +
                               to_string(e);
                    }
                    if (e.qty == 0) {
                        return "Accepted with zero quantity: " + to_string(e);
                    }
                    resting_[e.id] = {e.price, e.qty};
                    resting_total_ += e.qty;
                    break;
                }
                case ob::EventType::Trade: {
                    if (e.qty == 0) {
                        return "zero-quantity trade: " + to_string(e);
                    }
                    auto it = resting_.find(e.maker_id);
                    if (it == resting_.end()) {
                        return "Trade against a maker that is not resting: " +
                               to_string(e);
                    }
                    if (it->second.second < e.qty) {
                        return "Trade for more than the maker's remaining: " +
                               to_string(e);
                    }
                    if (it->second.first != e.price) {
                        return "Trade not at the maker's price: " + to_string(e);
                    }
                    it->second.second -= e.qty;
                    resting_total_ -= e.qty;
                    traded_total_ += e.qty;
                    if (it->second.second == 0) {
                        resting_.erase(it);
                    }
                    break;
                }
                case ob::EventType::Reduced: {
                    auto it = resting_.find(e.id);
                    if (it == resting_.end()) {
                        return "Reduced an id that is not resting: " +
                               to_string(e);
                    }
                    if (e.qty > it->second.second || e.qty == 0) {
                        return "Reduced to an illegal quantity: " + to_string(e);
                    }
                    resting_total_ -= it->second.second - e.qty;
                    it->second.second = e.qty;
                    break;
                }
                case ob::EventType::Cancelled: {
                    auto it = resting_.find(e.id);
                    if (it == resting_.end()) {
                        return "Cancelled an id that is not resting: " +
                               to_string(e);
                    }
                    if (it->second.second != e.qty) {
                        return "Cancelled quantity does not match remaining: " +
                               to_string(e);
                    }
                    resting_total_ -= e.qty;
                    cancelled_total_ += e.qty;
                    resting_.erase(it);
                    break;
                }
                case ob::EventType::Rejected:
                    if (e.reason == ob::RejectReason::None) {
                        return "Rejected without a reason: " + to_string(e);
                    }
                    break;
            }
        }
        return {};
    }

    // Returns "" or a description of the first divergence between the
    // ledger's expected state and the book's reported state.
    template <typename Book>
    std::string reconcile(const Book& book) const {
        if (book.order_count() != resting_.size()) {
            std::ostringstream os;
            os << "order_count " << book.order_count() << " != ledger "
               << resting_.size();
            return os.str();
        }
        ob::Quantity snap_total = 0;
        std::size_t snap_orders = 0;
        std::vector<ob::LevelView> rows(book.num_ticks());
        for (const ob::Side side : {ob::Side::Buy, ob::Side::Sell}) {
            const std::size_t n = book.snapshot(side, rows);
            for (std::size_t i = 0; i < n; ++i) {
                snap_total += rows[i].qty;
                snap_orders += rows[i].orders;
                if (book.quantity_at(side, rows[i].price) != rows[i].qty) {
                    return "quantity_at disagrees with snapshot";
                }
            }
        }
        if (snap_total != resting_total_) {
            std::ostringstream os;
            os << "snapshot total qty " << snap_total << " != ledger "
               << resting_total_;
            return os.str();
        }
        if (snap_orders != resting_.size()) {
            return "snapshot order count != ledger";
        }
        for (const auto& [id, pq] : resting_) {
            const auto q = book.open_quantity(id);
            if (!q || *q != pq.second) {
                std::ostringstream os;
                os << "open_quantity(" << id << ") disagrees with ledger";
                return os.str();
            }
        }
        return {};
    }

    [[nodiscard]] ob::Quantity traded_total() const { return traded_total_; }
    [[nodiscard]] std::size_t resting_count() const { return resting_.size(); }

private:
    std::unordered_map<ob::OrderId, std::pair<ob::Price, ob::Quantity>> resting_;
    ob::Quantity resting_total_{0};
    ob::Quantity traded_total_{0};
    ob::Quantity cancelled_total_{0};
};

// One randomized sequence against one engine: structural invariants after
// every operation, event legality after every operation, full reconciliation
// periodically and at the end. Returns "" on success.
template <typename Book>
std::string run_property_sequence(std::uint64_t seed, const BookParams& p,
                                  std::size_t ops) {
    Book book(p.base, p.num_ticks, p.capacity);
    FlowGen gen(seed, p);
    Ledger ledger;
    for (std::size_t i = 0; i < ops; ++i) {
        const Op op = gen.next();
        book.sink().events.clear();
        apply(book, op);
        std::ostringstream ctx;
        ctx << " [seed=" << seed << " op#" << i << " " << to_string(op) << "]";
        if (const char* err = book.validate()) {
            return std::string(err) + ctx.str();
        }
        if (std::string err = ledger.consume(book.sink().events); !err.empty()) {
            return err + ctx.str();
        }
        if (i % 16 == 15 || i + 1 == ops) {
            if (std::string err = ledger.reconcile(book); !err.empty()) {
                return err + ctx.str();
            }
        }
    }
    return {};
}

// Identical operation stream into both engines; the entire event streams must
// be value-identical after every operation, and the observable book state
// must agree at every checkpoint. Returns "" on success.
inline std::string run_differential(std::uint64_t seed, const BookParams& p,
                                    std::size_t ops,
                                    std::size_t check_every = 256) {
    ob::FastBook<ob::VectorSink> fast(p.base, p.num_ticks, p.capacity);
    ob::NaiveBook<ob::VectorSink> naive(p.base, p.num_ticks, p.capacity);
    FlowGen gen(seed, p);
    std::vector<ob::LevelView> rows_f(p.num_ticks);
    std::vector<ob::LevelView> rows_n(p.num_ticks);

    for (std::size_t i = 0; i < ops; ++i) {
        const Op op = gen.next();
        fast.sink().events.clear();
        naive.sink().events.clear();
        apply(fast, op);
        apply(naive, op);

        const auto& fe = fast.sink().events;
        const auto& ne = naive.sink().events;
        if (fe != ne) {
            std::ostringstream os;
            os << "event streams diverge at seed=" << seed << " op#" << i
               << " " << to_string(op) << ": fast emitted " << fe.size()
               << ", naive emitted " << ne.size();
            const std::size_t n = fe.size() < ne.size() ? fe.size() : ne.size();
            for (std::size_t k = 0; k <= n; ++k) {
                const std::string f =
                    k < fe.size() ? to_string(fe[k]) : "<none>";
                const std::string a =
                    k < ne.size() ? to_string(ne[k]) : "<none>";
                if (k == n || f != a) {
                    os << "; first difference at event " << k << ": fast="
                       << f << " naive=" << a;
                    break;
                }
            }
            return os.str();
        }

        if (i % check_every == check_every - 1 || i + 1 == ops) {
            std::ostringstream ctx;
            ctx << " [seed=" << seed << " op#" << i << "]";
            if (fast.best_bid() != naive.best_bid()) {
                return "best_bid diverges" + ctx.str();
            }
            if (fast.best_ask() != naive.best_ask()) {
                return "best_ask diverges" + ctx.str();
            }
            if (fast.order_count() != naive.order_count()) {
                return "order_count diverges" + ctx.str();
            }
            for (const ob::Side side : {ob::Side::Buy, ob::Side::Sell}) {
                const std::size_t nf = fast.snapshot(side, rows_f);
                const std::size_t nn = naive.snapshot(side, rows_n);
                if (nf != nn) {
                    return "snapshot depth diverges" + ctx.str();
                }
                for (std::size_t k = 0; k < nf; ++k) {
                    if (!(rows_f[k] == rows_n[k])) {
                        std::ostringstream os;
                        os << "snapshot row " << k << " diverges: fast={"
                           << rows_f[k].price << "," << rows_f[k].qty << ","
                           << rows_f[k].orders << "} naive={" << rows_n[k].price
                           << "," << rows_n[k].qty << "," << rows_n[k].orders
                           << "}" << ctx.str();
                        return os.str();
                    }
                }
            }
            if (const char* err = fast.validate()) {
                return std::string("fast invariant: ") + err + ctx.str();
            }
            if (const char* err = naive.validate()) {
                return std::string("naive invariant: ") + err + ctx.str();
            }
        }
    }
    return {};
}

}  // namespace obtest
