// Timer plumbing and configuration description for the benchmark harness.
// The flow driver itself is the template in generator.hpp.

#include "generator.hpp"

#include <chrono>
#include <sstream>

#include "ob/book_fast.hpp"

namespace obbench {

#if defined(__x86_64__) || defined(_M_X64)

std::uint64_t ticks() noexcept { return __rdtsc(); }
const char* timer_name() noexcept { return "rdtsc"; }

#else

std::uint64_t ticks() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}
const char* timer_name() noexcept { return "steady_clock"; }

#endif

// Measure the tick rate against steady_clock over a fixed window. Invariant
// TSCs on every remotely modern x86 make one calibration at startup enough.
double calibrate_ticks_per_ns() {
    using clock = std::chrono::steady_clock;
    const auto wall0 = clock::now();
    const std::uint64_t t0 = ticks();
    while (clock::now() - wall0 < std::chrono::milliseconds(200)) {
    }
    const std::uint64_t t1 = ticks();
    const auto wall1 = clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        wall1 - wall0)
                        .count();
    return static_cast<double>(t1 - t0) / static_cast<double>(ns);
}

namespace {

int event_code(ob::EventType t) {
    switch (t) {
        case ob::EventType::Accepted: return 0;
        case ob::EventType::Trade: return 1;
        case ob::EventType::Reduced: return 2;
        case ob::EventType::Cancelled: return 3;
        case ob::EventType::Rejected: return 4;
    }
    return -1;
}

std::uint64_t rrnd(std::mt19937_64& g, std::uint64_t n) { return g() % n; }

std::uint64_t rgeom(std::mt19937_64& g, std::uint64_t stop_percent,
                    std::uint64_t cap) {
    std::uint64_t k = 0;
    while (k < cap && rrnd(g, 100) >= stop_percent) {
        ++k;
    }
    return k;
}

}  // namespace

ReplayStats record_replay(const ReplayConfig& cfg, std::FILE* out) {
    const ob::Price lo = cfg.base;
    const ob::Price hi = cfg.base + static_cast<ob::Price>(cfg.num_ticks) - 1;
    const ob::Price mid = cfg.base + static_cast<ob::Price>(cfg.num_ticks / 2);
    const auto clamp = [lo, hi](ob::Price p) {
        return p < lo ? lo : (p > hi ? hi : p);
    };

    ob::FastBook<ob::VectorSink> book(cfg.base, cfg.num_ticks, cfg.capacity);
    LiveSet live;
    std::mt19937_64 rng(cfg.seed);
    ob::OrderId next_id = 1;
    ReplayStats stats;

    std::fprintf(out, "{\"base\":%lld,\"ticks\":%zu,\"seed\":%llu,\"ops\":[",
                 static_cast<long long>(cfg.base), cfg.num_ticks,
                 static_cast<unsigned long long>(cfg.seed));

    for (std::size_t i = 0; i < cfg.ops; ++i) {
        book.sink().events.clear();

        const std::uint64_t roll = rrnd(rng, 100);
        int kind = 0;  // 0 submit, 1 cancel, 2 modify
        ob::OrderId op_id = 0;
        ob::Side op_side = ob::Side::Buy;
        ob::OrderType op_type = ob::OrderType::Limit;
        ob::Price op_price = 0;
        ob::Quantity op_qty = 0;

        // Population guardrails keep a ladder on screen: refill under 60
        // resting orders, stop adding over 400.
        if (live.size() < 60 || (roll < 50 && live.size() < 400)) {
            kind = 0;
            op_id = next_id++;
            op_side = rrnd(rng, 2) == 0 ? ob::Side::Buy : ob::Side::Sell;
            const std::uint64_t t = rrnd(rng, 100);
            op_type = t < 84   ? ob::OrderType::Limit
                      : t < 92 ? ob::OrderType::IOC
                      : t < 97 ? ob::OrderType::Market
                               : ob::OrderType::FOK;
            const auto bb = book.best_bid();
            const auto ba = book.best_ask();
            const bool cross = rrnd(rng, 100) < 12;
            if (op_type == ob::OrderType::Market) {
                op_price = 0;
            } else if (rrnd(rng, 100) < 2) {
                op_price = hi + 5 + static_cast<ob::Price>(rrnd(rng, 20));
            } else if (cross || op_type == ob::OrderType::IOC ||
                       op_type == ob::OrderType::FOK) {
                const auto slip = static_cast<ob::Price>(rrnd(rng, 3));
                op_price = op_side == ob::Side::Buy
                               ? clamp(ba ? *ba + slip : mid)
                               : clamp(bb ? *bb - slip : mid);
            } else {
                const auto off =
                    static_cast<ob::Price>(rgeom(rng, 12, cfg.num_ticks / 8));
                op_price = op_side == ob::Side::Buy
                               ? clamp((bb ? *bb : mid) - off)
                               : clamp((ba ? *ba : mid) + off);
                if (op_side == ob::Side::Buy && ba && op_price >= *ba) {
                    op_price = clamp(*ba - 1);
                } else if (op_side == ob::Side::Sell && bb && op_price <= *bb) {
                    op_price = clamp(*bb + 1);
                }
            }
            op_qty = rrnd(rng, 100) < 6 ? 40 + rrnd(rng, 160)
                                        : 1 + rgeom(rng, 22, 60);
            book.submit(op_id, op_side, op_type, op_price, op_qty);
        } else if (roll < 88 || live.empty()) {
            kind = 1;
            op_id = live.pick(rng()).id;
            book.cancel(op_id);
        } else {
            const auto target = live.pick(rng());
            if (target.qty < 2) {
                kind = 1;
                op_id = target.id;
                book.cancel(op_id);
            } else {
                kind = 2;
                op_id = target.id;
                op_price = target.price;
                op_qty = 1 + rrnd(rng, target.qty - 1);
                book.modify(op_id, op_price, op_qty);
            }
        }

        std::fprintf(out,
                     "%s{\"k\":%d,\"id\":%llu,\"s\":%d,\"t\":%d,\"p\":%lld,"
                     "\"q\":%llu,\"e\":[",
                     i ? "," : "", kind,
                     static_cast<unsigned long long>(op_id),
                     static_cast<int>(op_side), static_cast<int>(op_type),
                     static_cast<long long>(op_price),
                     static_cast<unsigned long long>(op_qty));
        bool first = true;
        for (const ob::Event& e : book.sink().events) {
            std::fprintf(out, "%s[%d,%d,%d,%llu,%llu,%lld,%llu]",
                         first ? "" : ",", event_code(e.type),
                         static_cast<int>(e.side), static_cast<int>(e.reason),
                         static_cast<unsigned long long>(e.id),
                         static_cast<unsigned long long>(e.maker_id),
                         static_cast<long long>(e.price),
                         static_cast<unsigned long long>(e.qty));
            first = false;
            ++stats.events;
            stats.trades += e.type == ob::EventType::Trade;
            stats.rejects += e.type == ob::EventType::Rejected;
        }
        std::fprintf(out, "]}");

        live.on_events(book.sink().events);
        ++stats.ops;
    }
    std::fprintf(out, "]}\n");

    stats.resting = book.order_count();
    return stats;
}

std::string describe(const FlowConfig& cfg) {
    std::ostringstream os;
    os << "seed=" << cfg.seed << " cancel-ratio=" << cfg.cancel_ratio
       << " reduce-ratio=" << cfg.reduce_ratio
       << " aggressive-fraction=" << cfg.aggressive_fraction
       << " depth=" << cfg.depth << " band=[" << cfg.base << ","
       << cfg.base + static_cast<ob::Price>(cfg.num_ticks) << ")"
       << " capacity=" << cfg.capacity;
    return os.str();
}

}  // namespace obbench
