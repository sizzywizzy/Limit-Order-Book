#pragma once

// Synthetic order flow for the benchmark harness.
//
// Realistic means cancel-heavy (README.md, "Why this exists"): the mix is
// configurable but defaults to 90% cancels/reductions, prices concentrated
// near the touch and thinning outward, and log-normal-ish sizes so a minority
// of orders sweep several levels. The seed is a parameter and is recorded in
// the output, so any figure in RESULTS.md can be reproduced exactly.
//
// The driver keeps its own live-order table, maintained purely from the
// engine's event stream, so cancel and reduce targets are always real orders
// — a cancel that missed would measure the rejection path, not the cancel
// path. All bookkeeping happens OUTSIDE the timed window; the engine call is
// the only thing between the two timestamps. (The tracking table uses
// std::unordered_map deliberately: it is measurement scaffolding, not the
// engine, and it is never inside the timed region.)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "counters.hpp"
#include "ob/events.hpp"
#include "ob/types.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

namespace obbench {

// cancel_ratio is cancels PER ADD, not a share of messages: 0.9 means ~nine
// of every ten resting orders die by cancellation rather than by fill, which
// is what real order-to-trade ratios look like. A raw "90% of messages are
// cancels" cannot hold in steady state — removals would have to outrun
// additions — so that reading empties the book and benchmarks nothing. With
// this definition the message mix works out near adds 51% / cancels 46% /
// reduces 3%, and the book holds a stable cancel-dominated population.
struct FlowConfig {
    std::uint64_t seed{1};
    double cancel_ratio{0.90};        // cancels per add (see above)
    double aggressive_fraction{0.10}; // of submits, how many cross
    double reduce_ratio{0.05};        // in-place reductions per add
    std::size_t depth{50};            // target resting levels per side
    ob::Price base{100000};
    std::size_t num_ticks{16384};
    std::size_t capacity{1u << 16};
};

// Replay recording — the input to docs/dashboard.html.
//
// Same LiveSet-driven flow discipline as the latency workload, but tuned for
// a legible tape rather than a steady-state measurement: a small price band
// so a depth ladder fits on screen, more order-type variety, and a small
// fraction of deliberately out-of-band orders so the rejection path appears.
// It is a demonstration mix, not the benchmarked mix, and the dashboard says
// so. Nothing here is timed.
//
// Output is JSON: {base, ticks, seed, ops:[{k,id,s,t,p,q,e:[[...]]}]} where
// `e` holds the events the engine emitted for that operation, encoded as
// [type, side, reason, id, maker_id, price, qty]. The page rebuilds the whole
// book from those events alone — no depth snapshot is written, because the
// event stream is complete by construction (DECISIONS.md 008).
struct ReplayConfig {
    std::uint64_t seed{42};
    std::size_t ops{2600};
    ob::Price base{2400};
    std::size_t num_ticks{240};
    std::size_t capacity{512};
};

struct ReplayStats {
    std::size_t ops{0};
    std::size_t events{0};
    std::size_t trades{0};
    std::size_t rejects{0};
    std::size_t resting{0};
};

ReplayStats record_replay(const ReplayConfig& cfg, std::FILE* out);

// Timestamp source. rdtsc on x86-64 — steady_clock's ~100 ns granularity on
// Windows cannot resolve a ~20 ns cancel. Calibrated against steady_clock at
// startup; unserialised, so individual samples carry a few ns of jitter,
// which RESULTS.md lists under threats to validity.
std::uint64_t ticks() noexcept;
double calibrate_ticks_per_ns();
const char* timer_name() noexcept;

// Fixed-purpose event sink: appends to a pre-reserved buffer. No allocation
// once reserved (reserve() outlives every op; clear() keeps capacity).
struct BenchSink {
    std::vector<ob::Event> buf;
    void operator()(const ob::Event& e) { buf.push_back(e); }
};

// Live resting orders, reconstructed from events after each operation.
class LiveSet {
public:
    void on_events(const std::vector<ob::Event>& events) {
        for (const ob::Event& e : events) {
            switch (e.type) {
                case ob::EventType::Accepted:
                    add(e.id, e.price, e.qty);
                    break;
                case ob::EventType::Trade: {
                    auto it = info_.find(e.maker_id);
                    if (it != info_.end()) {
                        it->second.qty -= e.qty;
                        if (it->second.qty == 0) {
                            remove(e.maker_id);
                        }
                    }
                    break;
                }
                case ob::EventType::Reduced: {
                    auto it = info_.find(e.id);
                    if (it != info_.end()) {
                        it->second.qty = e.qty;
                    }
                    break;
                }
                case ob::EventType::Cancelled:
                    remove(e.id);
                    break;
                case ob::EventType::Rejected:
                    break;
            }
        }
    }

    [[nodiscard]] bool empty() const noexcept { return ids_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

    struct Entry {
        ob::OrderId id;
        ob::Price price;
        ob::Quantity qty;
    };

    [[nodiscard]] Entry pick(std::uint64_t r) const {
        const ob::OrderId id = ids_[r % ids_.size()];
        const auto& pq = info_.at(id);
        return Entry{id, pq.price, pq.qty};
    }

private:
    struct PriceQty {
        ob::Price price;
        ob::Quantity qty;
    };

    void add(ob::OrderId id, ob::Price price, ob::Quantity qty) {
        pos_[id] = ids_.size();
        ids_.push_back(id);
        info_[id] = PriceQty{price, qty};
    }

    void remove(ob::OrderId id) {
        const auto it = pos_.find(id);
        if (it == pos_.end()) {
            return;
        }
        const std::size_t p = it->second;
        ids_[p] = ids_.back();
        pos_[ids_[p]] = p;
        ids_.pop_back();
        pos_.erase(it);
        info_.erase(id);
    }

    std::vector<ob::OrderId> ids_;
    std::unordered_map<ob::OrderId, std::size_t> pos_;
    std::unordered_map<ob::OrderId, PriceQty> info_;
};

// Latency samples in raw ticks, one bucket per operation kind.
struct Samples {
    std::vector<std::uint32_t> cancel;
    std::vector<std::uint32_t> add_passive;
    std::vector<std::uint32_t> add_aggressive;
    std::vector<std::uint32_t> reduce;
};

struct RunStats {
    std::size_t executed{0};
    std::size_t trades{0};

    // Operation composition of the *measured* region (post warm-up), so
    // whole-region counter totals can be read per operation.
    std::size_t measured{0};
    std::size_t n_cancel{0};
    std::size_t n_add_passive{0};
    std::size_t n_add_aggressive{0};
    std::size_t n_reduce{0};

    CounterSet counters{};
    bool counted{false};
};

std::string describe(const FlowConfig& cfg);

namespace detail {
inline std::uint64_t rnd(std::mt19937_64& g, std::uint64_t n) {
    return g() % n;
}
inline std::uint64_t geometric(std::mt19937_64& g, std::uint64_t stop_percent,
                               std::uint64_t cap) {
    std::uint64_t k = 0;
    while (k < cap && rnd(g, 100) >= stop_percent) {
        ++k;
    }
    return k;
}
}  // namespace detail

// Drives `orders` operations into the book. The first `warmup` are executed
// but not recorded. The engine call is the only code between the timestamps.
template <typename Book>
RunStats run_flow(Book& book, const FlowConfig& cfg, std::size_t orders,
                  std::size_t warmup, Samples& out,
                  PerfCounters* counters = nullptr) {
    using detail::geometric;
    using detail::rnd;

    std::mt19937_64 rng(cfg.seed);
    LiveSet live;
    ob::OrderId next_id = 1;
    RunStats stats;

    book.sink().buf.reserve(16384);

    const ob::Price lo = cfg.base;
    const ob::Price hi = cfg.base + static_cast<ob::Price>(cfg.num_ticks) - 1;
    const ob::Price mid = cfg.base + static_cast<ob::Price>(cfg.num_ticks / 2);
    const auto clamp = [lo, hi](ob::Price p) {
        return p < lo ? lo : (p > hi ? hi : p);
    };
    const std::uint64_t passive_stop =
        cfg.depth > 0 ? (100 / cfg.depth > 2 ? 100 / cfg.depth : 2) : 20;

    const auto gen_qty = [&rng]() -> ob::Quantity {
        if (rnd(rng, 100) < 3) {
            return 50 + rnd(rng, 450);  // occasional sweeper
        }
        return 1 + geometric(rng, 25, 400);
    };

    // Normalise {add=1, cancel, reduce} weights into per-mille thresholds,
    // with population guardrails: below the floor the book refills, above the
    // ceiling it drains, and in between the configured mix runs freely.
    const double total_w = 1.0 + cfg.cancel_ratio + cfg.reduce_ratio;
    const auto cancel_upto =
        static_cast<std::uint64_t>(cfg.cancel_ratio / total_w * 1000.0);
    const auto reduce_upto =
        cancel_upto +
        static_cast<std::uint64_t>(cfg.reduce_ratio / total_w * 1000.0);
    const auto aggressive_upto = static_cast<std::uint64_t>(
        cfg.aggressive_fraction * 1000.0);
    const std::size_t min_live = cfg.depth * 4;
    const std::size_t max_live = cfg.capacity * 3 / 4;

    for (std::size_t i = 0; i < orders; ++i) {
        // Counters bracket the measured region only: warm-up runs outside
        // them, exactly as the discarded latency samples do.
        if (counters && i == warmup) {
            counters->start();
        }
        const std::uint64_t roll = rnd(rng, 1000);
        enum class Kind : std::uint8_t { Cancel, Reduce, Add } kind;
        if (live.size() < min_live) {
            kind = Kind::Add;
        } else if (live.size() > max_live) {
            kind = Kind::Cancel;
        } else if (roll < cancel_upto) {
            kind = Kind::Cancel;
        } else if (roll < reduce_upto) {
            kind = Kind::Reduce;
        } else {
            kind = Kind::Add;
        }

        book.sink().buf.clear();
        std::uint64_t t0 = 0;
        std::uint64_t t1 = 0;

        if (kind == Kind::Cancel) {
            const auto target = live.pick(rng());
            t0 = ticks();
            book.cancel(target.id);
            t1 = ticks();
            if (i >= warmup) {
                out.cancel.push_back(
                    static_cast<std::uint32_t>(t1 - t0));
                ++stats.n_cancel;
            }
        } else if (kind == Kind::Reduce) {
            const auto target = live.pick(rng());
            if (target.qty < 2) {
                // Nothing to shave off; count it as a cancel instead.
                t0 = ticks();
                book.cancel(target.id);
                t1 = ticks();
                if (i >= warmup) {
                    out.cancel.push_back(
                        static_cast<std::uint32_t>(t1 - t0));
                    ++stats.n_cancel;
                }
            } else {
                const ob::Quantity new_qty = 1 + rnd(rng, target.qty - 1);
                t0 = ticks();
                book.modify(target.id, target.price, new_qty);
                t1 = ticks();
                if (i >= warmup) {
                    out.reduce.push_back(
                        static_cast<std::uint32_t>(t1 - t0));
                    ++stats.n_reduce;
                }
            }
        } else {
            const ob::Side side =
                rnd(rng, 2) == 0 ? ob::Side::Buy : ob::Side::Sell;
            const bool want_cross = rnd(rng, 1000) < aggressive_upto;
            const auto bb = book.best_bid();
            const auto ba = book.best_ask();
            ob::Price price;
            if (want_cross) {
                const auto slip = static_cast<ob::Price>(rnd(rng, 3));
                price = side == ob::Side::Buy
                            ? clamp(ba ? *ba + slip : mid)
                            : clamp(bb ? *bb - slip : mid);
            } else {
                const auto off = static_cast<ob::Price>(
                    geometric(rng, passive_stop, cfg.num_ticks / 4));
                price = side == ob::Side::Buy
                            ? clamp((bb ? *bb : mid) - off)
                            : clamp((ba ? *ba : mid) + off);
                // Stay passive when the offset would land on or through the
                // opposite touch.
                if (side == ob::Side::Buy && ba && price >= *ba) {
                    price = clamp(*ba - 1);
                } else if (side == ob::Side::Sell && bb && price <= *bb) {
                    price = clamp(*bb + 1);
                }
            }
            const ob::Quantity qty = gen_qty();
            const ob::OrderId id = next_id++;
            t0 = ticks();
            book.submit(id, side, ob::OrderType::Limit, price, qty);
            t1 = ticks();

            bool traded = false;
            for (const ob::Event& e : book.sink().buf) {
                if (e.type == ob::EventType::Trade) {
                    traded = true;
                    ++stats.trades;
                }
            }
            if (i >= warmup) {
                (traded ? out.add_aggressive : out.add_passive)
                    .push_back(static_cast<std::uint32_t>(t1 - t0));
                ++(traded ? stats.n_add_aggressive : stats.n_add_passive);
            }
        }

        live.on_events(book.sink().buf);
        ++stats.executed;
    }
    if (counters) {
        stats.counters = counters->stop();
        stats.counted = counters->available();
    }
    stats.measured = stats.n_cancel + stats.n_add_passive +
                     stats.n_add_aggressive + stats.n_reduce;
    return stats;
}

}  // namespace obbench
