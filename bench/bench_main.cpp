// Benchmark harness, and the recorder behind docs/dashboard.html.
//
//   ob_bench --engine={naive|fast} --orders=N [--cancel-ratio=R]
//            [--aggressive-fraction=F] [--reduce-ratio=R] [--depth=L]
//            [--seed=S] [--warmup=W] [--out=FILE]
//
//   ob_bench --replay-out=FILE [--replay-ops=N] [--seed=S]
//            [--replay-base=P] [--replay-ticks=N] [--replay-capacity=N]
//
// Output contract (consumed by scripts/plot.py — keep the two in step):
//
//     engine,operation,latency_ns,seed
//     naive,cancel,412,1
//     fast,cancel,38,1
//
// One row per measured operation, long format, no aggregation in C++.
// Percentiles are computed in the plot script so the raw sample set stays
// available and a reviewer can recompute anything. The `seed` column lets
// the script report run-to-run spread from a set of CSVs without inferring
// anything from filenames; it is additive, so a 3-column CSV still loads.
//
// Measurement rules honoured here (RESULTS.md, "Measurement rules"): warm-up
// operations are executed but not recorded; every operation type is recorded
// separately (cancel, add_passive, add_aggressive, reduce — adds classified
// by whether they actually traded); the seed is a parameter and is printed;
// the timer and its calibration are printed. Thread pinning and the
// median-of-5-runs rule belong to the runner: invoke this binary 5+ times and
// aggregate outside, so no run can quietly average itself flattering.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>

#include "generator.hpp"
#include "ob/book_fast.hpp"
#include "ob/book_naive.hpp"

namespace {

struct Args {
    std::string engine;
    std::size_t orders = 0;
    std::size_t warmup = 0;  // 0 → orders/10
    std::string out;
    obbench::FlowConfig cfg;
    std::string replay_out;  // non-empty ⇒ replay recording mode
    obbench::ReplayConfig replay;
    bool ok = false;
};

bool parse_num(const char* s, std::size_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    out = static_cast<std::size_t>(v);
    return true;
}

bool parse_real(const char* s, double& out) {
    char* end = nullptr;
    const double v = std::strtod(s, &end);
    if (end == s || *end != '\0' || v < 0.0 || v > 1.0) {
        return false;
    }
    out = v;
    return true;
}

Args parse(int argc, char** argv) {
    Args a;
    a.ok = true;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* val = nullptr;
        const auto is = [arg, &val](const char* prefix) {
            const std::size_t n = std::strlen(prefix);
            if (std::strncmp(arg, prefix, n) == 0) {
                val = arg + n;
                return true;
            }
            return false;
        };
        std::size_t tmp = 0;
        if (is("--engine=")) {
            a.engine = val;
        } else if (is("--orders=")) {
            a.ok = a.ok && parse_num(val, a.orders);
        } else if (is("--warmup=")) {
            a.ok = a.ok && parse_num(val, a.warmup);
        } else if (is("--cancel-ratio=")) {
            a.ok = a.ok && parse_real(val, a.cfg.cancel_ratio);
        } else if (is("--aggressive-fraction=")) {
            a.ok = a.ok && parse_real(val, a.cfg.aggressive_fraction);
        } else if (is("--reduce-ratio=")) {
            a.ok = a.ok && parse_real(val, a.cfg.reduce_ratio);
        } else if (is("--depth=")) {
            a.ok = a.ok && parse_num(val, a.cfg.depth);
        } else if (is("--seed=")) {
            a.ok = a.ok && parse_num(val, tmp);
            a.cfg.seed = tmp;
            a.replay.seed = tmp;
        } else if (is("--out=")) {
            a.out = val;
        } else if (is("--replay-out=")) {
            a.replay_out = val;
        } else if (is("--replay-ops=")) {
            a.ok = a.ok && parse_num(val, a.replay.ops);
        } else if (is("--replay-ticks=")) {
            a.ok = a.ok && parse_num(val, a.replay.num_ticks);
        } else if (is("--replay-capacity=")) {
            a.ok = a.ok && parse_num(val, a.replay.capacity);
        } else if (is("--replay-base=")) {
            a.ok = a.ok && parse_num(val, tmp);
            a.replay.base = static_cast<ob::Price>(tmp);
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg);
            a.ok = false;
        }
    }
    if (!a.replay_out.empty()) {
        // Replay recording is its own mode: no engine choice (the dashboard
        // replays the optimised engine) and no latency measurement.
        if (a.replay.ops == 0 || a.replay.num_ticks == 0 ||
            a.replay.capacity == 0) {
            a.ok = false;
        }
        return a;
    }
    if (a.engine != "naive" && a.engine != "fast") {
        a.ok = false;
    }
    if (a.orders == 0) {
        a.ok = false;
    }
    if (a.warmup == 0) {
        a.warmup = a.orders / 10;
    }
    return a;
}

void usage() {
    std::fputs(
        "usage:\n"
        "  ob_bench --engine={naive|fast} --orders=N [--cancel-ratio=R]\n"
        "           [--aggressive-fraction=F] [--reduce-ratio=R] [--depth=L]\n"
        "           [--seed=S] [--warmup=W] [--out=FILE]\n"
        "\n"
        "  ob_bench --replay-out=FILE [--replay-ops=N] [--seed=S]\n"
        "           [--replay-base=P] [--replay-ticks=N] [--replay-capacity=N]\n"
        "\n"
        "Benchmark mode writes one CSV row per measured operation:\n"
        "  engine,operation,latency_ns,seed\n"
        "Replay mode writes the JSON the dashboard replays (see\n"
        "scripts/build_dashboard.py).\n",
        stderr);
}

void write_samples(std::FILE* f, const char* engine, const char* operation,
                   const std::vector<std::uint32_t>& ticks,
                   double ticks_per_ns, std::uint64_t seed) {
    for (const std::uint32_t t : ticks) {
        const auto ns = static_cast<long long>(
            static_cast<double>(t) / ticks_per_ns + 0.5);
        std::fprintf(f, "%s,%s,%lld,%llu\n", engine, operation, ns,
                     static_cast<unsigned long long>(seed));
    }
}

int run_replay(const Args& a) {
    std::FILE* f = std::fopen(a.replay_out.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", a.replay_out.c_str());
        return 1;
    }
    const obbench::ReplayStats s = obbench::record_replay(a.replay, f);
    std::fclose(f);
    std::printf("replay: seed=%llu ops=%zu events=%zu trades=%zu rejects=%zu "
                "resting=%zu band=[%lld,%lld)\n",
                static_cast<unsigned long long>(a.replay.seed), s.ops,
                s.events, s.trades, s.rejects, s.resting,
                static_cast<long long>(a.replay.base),
                static_cast<long long>(a.replay.base) +
                    static_cast<long long>(a.replay.num_ticks));
    std::printf("wrote %s\n", a.replay_out.c_str());
    return 0;
}

template <typename Book>
int run(const Args& a) {
    const double ticks_per_ns = obbench::calibrate_ticks_per_ns();
    std::printf("engine=%s orders=%zu warmup=%zu\n", a.engine.c_str(),
                a.orders, a.warmup);
    std::printf("%s\n", obbench::describe(a.cfg).c_str());
    std::printf("timer=%s ticks_per_ns=%.3f\n", obbench::timer_name(),
                ticks_per_ns);

    Book book(a.cfg.base, a.cfg.num_ticks, a.cfg.capacity);
    obbench::Samples samples;
    samples.cancel.reserve(a.orders);
    samples.add_passive.reserve(a.orders);
    samples.add_aggressive.reserve(a.orders);
    samples.reduce.reserve(a.orders);

    const auto wall0 = std::chrono::steady_clock::now();
    const obbench::RunStats stats =
        obbench::run_flow(book, a.cfg, a.orders, a.warmup, samples);
    const auto wall1 = std::chrono::steady_clock::now();
    const double secs =
        std::chrono::duration<double>(wall1 - wall0).count();

    std::printf("executed=%zu trades=%zu resting=%zu wall=%.3fs "
                "throughput=%.0f ops/s\n",
                stats.executed, stats.trades, book.order_count(), secs,
                static_cast<double>(stats.executed) / secs);
    std::printf("samples: cancel=%zu add_passive=%zu add_aggressive=%zu "
                "reduce=%zu\n",
                samples.cancel.size(), samples.add_passive.size(),
                samples.add_aggressive.size(), samples.reduce.size());

    if (!a.out.empty()) {
        std::FILE* f = std::fopen(a.out.c_str(), "w");
        if (!f) {
            std::fprintf(stderr, "cannot open %s\n", a.out.c_str());
            return 1;
        }
        std::fputs("engine,operation,latency_ns,seed\n", f);
        const char* e = a.engine.c_str();
        const std::uint64_t s = a.cfg.seed;
        write_samples(f, e, "cancel", samples.cancel, ticks_per_ns, s);
        write_samples(f, e, "add_passive", samples.add_passive, ticks_per_ns, s);
        write_samples(f, e, "add_aggressive", samples.add_aggressive,
                      ticks_per_ns, s);
        write_samples(f, e, "reduce", samples.reduce, ticks_per_ns, s);
        std::fclose(f);
        std::printf("wrote %s\n", a.out.c_str());
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args a = parse(argc, argv);
    if (!a.ok) {
        usage();
        return 1;
    }
    if (!a.replay_out.empty()) {
        return run_replay(a);
    }
    if (a.engine == "fast") {
        return run<ob::FastBook<obbench::BenchSink>>(a);
    }
    return run<ob::NaiveBook<obbench::BenchSink>>(a);
}
