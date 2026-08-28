// Dependency-free test runner: the same harness and scenario logic the Catch2
// layers use, runnable with nothing but a compiler:
//
//     g++ -std=c++23 -O2 -I include tests/standalone_runner.cpp -o ob_check
//     ./ob_check [prop_seqs] [prop_ops] [diff_ops]
//
// Defaults are sized for CI; the phase definitions of done (10,000+ property
// sequences, 1,000,000+ differential operations) are a matter of passing
// bigger arguments:  ./ob_check 10000 300 1000000
//
// Layers run here: component fuzz (TieredBitmap and IdIndex against obvious
// reference models), the unit scenario set against BOTH engines, randomized
// property sequences against both engines, the naive-vs-fast differential
// stream comparison, deterministic replay, and the steady-state
// zero-allocation proof for the fast engine.

#include "alloc_counter.hpp"  // must precede any allocation in this TU

#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "harness.hpp"
#include "ob/bitmap.hpp"
#include "ob/book_fast.hpp"
#include "ob/book_naive.hpp"
#include "ob/id_index.hpp"
#include "scenarios.hpp"

namespace {

struct Reporter {
    int passed = 0;
    int failed = 0;

    void check(bool ok, const std::string& what) {
        if (ok) {
            ++passed;
        } else {
            ++failed;
            std::printf("FAIL: %s\n", what.c_str());
        }
    }

    // For harness runners returning "" on success.
    void check_empty(const std::string& err, const std::string& what) {
        check(err.empty(), what + (err.empty() ? "" : (": " + err)));
    }
};

// ---------------------------------------------------------------------------
// TieredBitmap fuzz against a flat reference model
// ---------------------------------------------------------------------------

void fuzz_bitmap(Reporter& rep) {
    const std::size_t sizes[] = {1, 63, 64, 65, 300, 4096, 20000};
    std::mt19937_64 rng(42);
    for (const std::size_t n : sizes) {
        ob::TieredBitmap bm(n);
        std::vector<char> ref(n, 0);
        bool ok = true;
        std::string first_err;
        const std::size_t ops = 2000;
        for (std::size_t k = 0; k < ops && ok; ++k) {
            const auto i = static_cast<std::uint32_t>(obtest::rnd(rng, n));
            if (obtest::rnd(rng, 2) == 0) {
                bm.set(i);
                ref[i] = 1;
            } else {
                bm.clear(i);
                ref[i] = 0;
            }
            // Reference answers by linear scan.
            std::uint32_t lo = ob::npos32;
            std::uint32_t hi = ob::npos32;
            for (std::uint32_t j = 0; j < n; ++j) {
                if (ref[j]) {
                    if (lo == ob::npos32) {
                        lo = j;
                    }
                    hi = j;
                }
            }
            if (bm.first() != lo || bm.last() != hi) {
                ok = false;
                first_err = "first/last mismatch n=" + std::to_string(n);
                break;
            }
            for (int probe = 0; probe < 2; ++probe) {
                const auto q = static_cast<std::uint32_t>(obtest::rnd(rng, n));
                std::uint32_t above = ob::npos32;
                for (std::uint32_t j = q + 1; j < n; ++j) {
                    if (ref[j]) {
                        above = j;
                        break;
                    }
                }
                std::uint32_t below = ob::npos32;
                for (std::uint32_t j = q; j-- > 0;) {
                    if (ref[j]) {
                        below = j;
                        break;
                    }
                }
                if (bm.next_above(q) != above || bm.next_below(q) != below ||
                    bm.test(q) != (ref[q] != 0)) {
                    ok = false;
                    first_err = "next_above/next_below/test mismatch n=" +
                                std::to_string(n) + " q=" + std::to_string(q);
                    break;
                }
            }
        }
        rep.check(ok, "bitmap fuzz n=" + std::to_string(n) +
                          (ok ? "" : (": " + first_err)));
    }
}

// ---------------------------------------------------------------------------
// IdIndex fuzz against std::unordered_map
// ---------------------------------------------------------------------------

void fuzz_id_index(Reporter& rep) {
    constexpr std::size_t cap = 4096;
    ob::IdIndex ix(cap);
    std::unordered_map<ob::OrderId, std::uint32_t> ref;
    std::vector<ob::OrderId> present;
    std::mt19937_64 rng(7);
    bool ok = true;
    std::string first_err;

    for (std::size_t k = 0; k < 200000 && ok; ++k) {
        const std::uint64_t roll = obtest::rnd(rng, 100);
        if (roll < 50 && ref.size() < cap) {
            const ob::OrderId key = 1 + obtest::rnd(rng, 10000);
            if (!ref.contains(key)) {
                const auto val =
                    static_cast<std::uint32_t>(obtest::rnd(rng, 1u << 20));
                ix.insert(key, val);
                ref.emplace(key, val);
                present.push_back(key);
            }
        } else if (roll < 80 && !present.empty()) {
            const std::size_t p = obtest::rnd(rng, present.size());
            const ob::OrderId key = present[p];
            present[p] = present.back();
            present.pop_back();
            if (!ix.erase(key)) {
                ok = false;
                first_err = "erase of present key failed";
                break;
            }
            ref.erase(key);
        } else {
            const ob::OrderId key = 20000 + obtest::rnd(rng, 10000);
            if (ix.erase(key) || ix.find(key) != ob::npos32) {
                ok = false;
                first_err = "absent key reported present";
                break;
            }
        }
        for (int probe = 0; probe < 3; ++probe) {
            const ob::OrderId key = 1 + obtest::rnd(rng, 10000);
            const auto it = ref.find(key);
            const std::uint32_t expect =
                it == ref.end() ? ob::npos32 : it->second;
            if (ix.find(key) != expect) {
                ok = false;
                first_err = "find mismatch key=" + std::to_string(key);
                break;
            }
        }
        if (ix.size() != ref.size()) {
            ok = false;
            first_err = "size mismatch";
        }
    }
    rep.check(ok, std::string("id index fuzz") + (ok ? "" : (": " + first_err)));
}

// ---------------------------------------------------------------------------
// Deterministic replay: same seed, fresh book, identical stream
// ---------------------------------------------------------------------------

void replay_check(Reporter& rep, std::size_t ops) {
    const obtest::BookParams p;
    std::vector<obtest::Op> flow;
    flow.reserve(ops);
    obtest::FlowGen gen(2024, p);
    for (std::size_t i = 0; i < ops; ++i) {
        flow.push_back(gen.next());
    }
    auto run = [&] {
        ob::FastBook<ob::VectorSink> book(p.base, p.num_ticks, p.capacity);
        for (const auto& op : flow) {
            obtest::apply(book, op);
        }
        return std::move(book.sink().events);
    };
    const auto a = run();
    const auto b = run();
    rep.check(a == b && !a.empty(),
              "deterministic replay: identical input, identical stream (" +
                  std::to_string(a.size()) + " events)");
}

// ---------------------------------------------------------------------------
// Zero allocation in the steady state (fast engine)
// ---------------------------------------------------------------------------

void zero_alloc_check(Reporter& rep, std::size_t ops) {
    const obtest::BookParams p{1000, 2048, 4096};
    std::vector<obtest::Op> flow;
    flow.reserve(ops * 2);
    obtest::FlowGen gen(9001, p);
    for (std::size_t i = 0; i < ops * 2; ++i) {
        flow.push_back(gen.next());
    }

    ob::FastBook<ob::NullSink> book(p.base, p.num_ticks, p.capacity);
    for (std::size_t i = 0; i < ops; ++i) {  // warm-up half
        obtest::apply(book, flow[i]);
    }
    const std::uint64_t before = obtest::allocations();
    for (std::size_t i = ops; i < flow.size(); ++i) {
        obtest::apply(book, flow[i]);
    }
    const std::uint64_t after = obtest::allocations();

    if (!obtest::alloc_counting_enabled) {
        std::printf("note: allocation counting disabled (sanitizer build); "
                    "zero-alloc check skipped\n");
        return;
    }
    rep.check(after == before,
              "fast engine steady state allocates nothing (" +
                  std::to_string(after - before) + " allocations over " +
                  std::to_string(ops) + " ops)");
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t prop_seqs =
        argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 400;
    const std::size_t prop_ops =
        argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 250;
    const std::size_t diff_ops =
        argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 200000;

    Reporter rep;

    fuzz_bitmap(rep);
    fuzz_id_index(rep);

    const auto check = [&rep](bool ok, const std::string& what) {
        rep.check(ok, what);
    };
    obtest::unit_scenarios<ob::FastBook<ob::VectorSink>>(check, "fast");
    obtest::unit_scenarios<ob::NaiveBook<ob::VectorSink>>(check, "naive");

    {  // property sequences, both engines, distinct seeds per sequence
        std::string err;
        std::size_t done = 0;
        for (std::size_t s = 0; s < prop_seqs && err.empty(); ++s) {
            err = obtest::run_property_sequence<ob::FastBook<ob::VectorSink>>(
                1000 + s, obtest::BookParams{}, prop_ops);
            if (err.empty()) {
                err = obtest::run_property_sequence<
                    ob::NaiveBook<ob::VectorSink>>(1000 + s,
                                                   obtest::BookParams{},
                                                   prop_ops);
            }
            ++done;
        }
        rep.check_empty(err, "property sequences x" + std::to_string(done) +
                                 " (" + std::to_string(prop_ops) +
                                 " ops each, both engines)");
    }

    rep.check_empty(
        obtest::run_differential(31337, obtest::BookParams{}, diff_ops),
        "differential naive-vs-fast over " + std::to_string(diff_ops) + " ops");

    replay_check(rep, 50000);
    zero_alloc_check(rep, 100000);

    std::printf("%s: %d checks passed, %d failed\n",
                rep.failed == 0 ? "OK" : "FAILED", rep.passed, rep.failed);
    return rep.failed == 0 ? 0 : 1;
}
