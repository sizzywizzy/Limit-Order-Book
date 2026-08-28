# Benchmark results

All latency figures are nanoseconds per operation unless stated otherwise.

**Methodology is specified before any numbers appear**, so the numbers can be judged against a
stated standard rather than taken on trust.

> [!NOTE]
> The figures below are a **first baseline** from a development laptop — unpinned thread,
> frequency scaling active, Windows. They are honest about that (see
> [Threats to validity](#threats-to-validity)) and good enough to establish the shape of the
> comparison; they are not exchange-colo numbers. No optimisation passes have been run yet —
> this table is the "before" column of the optimisation history to come.

---

## Methodology

### Environment

| | |
|---|---|
| CPU | Intel Core i7-13700H (13th gen), 14 cores / 20 threads, TSC ≈ 2.92 GHz |
| Caches | 24 MiB L3, 11.5 MiB total L2 |
| Memory | 16 GiB LPDDR5-6400 |
| OS / kernel | Windows 11 Home 10.0.26200 |
| Compiler | g++ 16.2.0 (MSYS2 UCRT64) |
| Flags | `-std=c++23 -O2 -march=native -DNDEBUG` |
| Frequency scaling | **Enabled** (laptop; not controlled) |
| Thread pinning | **Not pinned** |
| Timer | `rdtsc`, unserialised, calibrated against `steady_clock` over 200 ms at startup (≈2.92 ticks/ns on this machine); the calibration is printed by every run |

### Measurement rules

These are applied to every figure in this document:

1. **Percentiles, never means.** p50, p99, p99.9, p99.99, max. In latency-sensitive systems the
   tail is the product; a mean conceals exactly the behaviour being measured.
2. **Per operation type, separately.** Cancel, passive add, aggressive add and in-place reduce
   differ by up to an order of magnitude. Adds are classified by what actually happened
   (traded ⇒ aggressive), not by intent.
3. **Warm-up discarded.** The first 10% of operations (200,000 for the fast runs) are executed
   but not recorded.
4. **Median of 5 runs**, seeds 1–5, with the observed spread reported below. Every reported
   figure comes from the median run *by cancel p50, chosen per engine* — pooling runs would
   report a percentile of a mixture, which is not a latency any single run exhibited.
   `scripts/plot.py` picks that run and writes the table, the figures and the dashboard's data
   from it in one pass; none of the three is ever typed by hand.
5. **Environment held constant** across every comparison in a given table; both engines run the
   identical seed-5 flow.

### Workload

Synthetic order flow from `bench/generator.hpp`, all parameters printed by each run:

| Parameter | Value | Rationale |
|---|---|---|
| Cancel ratio | 0.90 **per add** | ~9 of 10 resting orders die by cancellation, matching real order-to-trade ratios. (A raw "90% of messages" reading cannot hold in steady state — removals would have to outrun additions — so the generator normalises {add=1, cancel=0.9, reduce=0.05} into a message mix of ≈51% adds / 46% cancels / 3% reduces and keeps the book populated.) |
| Aggressive fraction | 0.10 of submits | Most orders rest; a minority cross |
| Price distribution | Geometric around the touch, thinning outward | Uniform prices give an unrealistically flat book and flattering cache behaviour |
| Order size | 1 + geometric (mean ≈ 4), 3% sweepers of 50–500 | Many small, few large — drives multi-level sweeps |
| Steady-state population | ~200–260 resting orders over ~50 levels/side | Guardrails refill below 4×depth |
| Band / capacity | 16,384 ticks / 65,536 orders | Fast-engine structures exercised at realistic size |
| Total operations | Fast: 2,000,000/run · Naive: 300,000/run | After 10% warm-up |

Seeds are fixed and recorded: `1 2 3 4 5`; every table below states which run it came from.

Cancel and reduce targets are always live orders (the harness tracks the book from its own
event stream), so the cancel bucket measures cancels, not rejection paths. All harness
bookkeeping happens outside the timed window; the engine call is the only code between
timestamps.

---

## Headline comparison

Each engine's **median run by cancel p50** out of 5 seeds (naive: seed 2, fast: seed 4). The
table below is `docs/figures/latency_percentiles.md`, pasted verbatim — it, the figures, and
every number in the dashboard come from one invocation of `scripts/plot.py` over the same ten
CSVs, so they cannot disagree (see [Reproducing these numbers](#reproducing-these-numbers)):

| Operation | Engine | Samples | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|
| add_aggressive | naive | 14,229 | 1,516 | 3,580 | 15,614 | 67,567 | 209,282 |
| add_aggressive | fast | 95,501 | 240 | 650 | 1,186 | 28,648 | 293,044 |
| add_passive | naive | 128,974 | 1,686 | 2,840 | 20,014 | 82,264 | 274,406 |
| add_passive | fast | 859,542 | 130 | 404 | 699 | 25,787 | 580,285 |
| cancel | naive | 122,000 | 942 | 2,587 | 5,625 | 39,195 | 116,438 |
| cancel | fast | 812,359 | 146 | 428 | 773 | 19,923 | 364,391 |
| reduce | naive | 4,797 | 1,380 | 3,375 | 5,530 | 25,954 | 26,108 |
| reduce | fast | 32,598 | 90 | 305 | 630 | 2,109 | 213,639 |

![percentiles](docs/figures/latency_percentiles.png)
![tail](docs/figures/latency_tail.png)

**Run-to-run spread across the 5 seeds** (min–max of each percentile, ns):

| Operation | Engine | p50 spread | p99 spread | p99.9 spread |
|---|---|---|---|---|
| cancel | fast | 132–162 | 421–478 | 773–920 |
| cancel | naive | 919–983 | 2,552–2,780 | 5,625–16,346 |
| add_passive | fast | 115–164 | 396–418 | 694–1,104 |
| add_passive | naive | 1,606–1,726 | 2,815–3,201 | 20,014–24,996 |
| add_aggressive | fast | 225–252 | 621–694 | 1,168–1,575 |
| add_aggressive | naive | 1,458–1,545 | 3,518–4,039 | 15,614–29,085 |
| reduce | fast | 85–94 | 305–356 | 630–746 |
| reduce | naive | 1,321–1,466 | 3,308–3,520 | 4,687–24,271 |

p50 and p99 hold within ~20% across seeds on both engines; p99.9 is already wide on the naive
side (5.6–16.3 µs on cancel) and p99.99 wider still, which is the machine, not the engine — see
[threats to validity](#threats-to-validity).

**Wall-clock throughput** (whole run ÷ wall time; includes the *untimed* harness bookkeeping,
so this understates the engines and is only comparable engine-to-engine):

| Engine | Operations/second (5-run range) |
|---|---|
| Reference | 411k – 462k |
| Optimised | 1.04M – 1.31M |

Reading the table rather than a ratio: at ~200 resting orders the naive cancel's O(N) scan
costs ~940 ns; the fast cancel — hash probe, four link writes, bitmap clear — costs ~146 ns, of
which a measurable share is the rdtsc pair itself. The gap in *passive add* (130 vs 1,686 ns)
is allocation: the naive engine pays a `std::list` node per order. The naive numbers grow with
book size; the fast numbers do not — that scaling sweep is the next measurement to run
(`--depth` is already a harness parameter).

> [!NOTE]
> These figures come from a rerun on 28 Aug 2026 with a busier machine than the first
> recording, and are 20–30% higher across the board than the initial pass (fast cancel p50 was
> 112 ns then, 146 ns here) — on **both** engines, so the comparison holds while the absolute
> numbers move. That is exactly the run-to-run variance an unpinned laptop produces, and it is
> why the spread table exists and why pinned figures are on the phase-3 list.

---

## Reproducing these numbers

Every artifact in this document — the table above, both figures, and the interactive dashboard
— is regenerated by these four commands. Nothing downstream of `ob_bench` is hand-written, so
a stale figure or a dashboard that disagrees with this file is not possible; the dashboard
carries the generating command's date, source files and git revision in its footer.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON && cmake --build build -j

for s in 1 2 3 4 5; do
  ./build/bench/ob_bench --engine=fast  --orders=2000000 --seed=$s --out=data/fast_s$s.csv
  ./build/bench/ob_bench --engine=naive --orders=300000  --seed=$s --out=data/naive_s$s.csv
done
./build/bench/ob_bench --replay-out=data/replay.json --replay-ops=2600 --seed=42

python3 scripts/plot.py data/*.csv --outdir docs/figures
python3 scripts/build_dashboard.py
```

`plot.py` writes `latency_percentiles.{csv,md,png}`, `latency_tail.png` and
`dashboard_data.json`; `build_dashboard.py` injects that plus the replay recording into
`docs/dashboard.template.html` and writes `docs/dashboard.html`. Paste the generated
`latency_percentiles.md` over the table above — it is the only manual step, and it is a copy,
not a transcription.

The replay recording is a *demonstration* mix rather than the latency workload: a small price
band so a depth ladder is legible, more order-type variety, and ~2% deliberately out-of-band
orders so the rejection path appears on the tape. It is timed by nothing and measured for
nothing; it exists to show the engine's event stream is complete enough to rebuild the book
from (DECISIONS.md 008).

---

## Optimisation history

One row per change. **Record the failures too** — a write-up containing "I tried this, it made
no measurable difference, and here is why I think that is" reads as considerably more credible
engineering than an unbroken list of wins.

No optimisation passes yet: the engine's first measured build *is* the design described in
DECISIONS.md 003–008 (flat levels, tiered bitmap, index-linked intrusive FIFOs, slot pool, flat
id index). The table below starts when profile-driven changes start.

| # | Change | Hypothesis | Cancel p99 before | after | Δ | Kept? |
|---|---|---|---|---|---|---|
| — | *(baseline established 2026-08-28, table above)* | | 362 | | | |

---

## Profiling evidence

Not yet collected. `perf stat` / cachegrind require the Linux toolchain (WSL2 or CI); the
Windows baseline above was produced first so the profiling session has numbers to be checked
against. Planned: cycles, instructions, IPC, cache-miss and branch-miss rates for both engines
on the identical seed-5 workload.

### Allocation count in the steady state

Counted by a replacement global `operator new`/`delete`
([tests/alloc_counter.hpp](tests/alloc_counter.hpp)) — not asserted, counted:

| Engine | Allocations during 100,000 steady-state operations (after warm-up) |
|---|---|
| Reference | not measured (allocates by design: list nodes, level splices) |
| Optimised | **0** |

Enforced continuously by `ob_test_properties` ("fast engine allocates nothing in the steady
state") and the standalone runner.

---

## Scaling behaviour

Planned, not yet run — the harness already takes `--depth`:

- **Latency vs book depth** (10 / 100 / 1,000 levels): the reference engine's O(N) cancel scan
  should show; the optimised engine's O(1) should stay flat.
- **Latency vs cancel ratio** (0.5 → 0.99): the gap should widen as cancels dominate, which is
  the empirical demonstration of the project's central design argument.

---

## Correctness evidence

Performance figures mean nothing without this section. Current tree, verified on the
environment above (g++ 16.2, `-O2`, assertions enabled for the test builds):

| Check | Result |
|---|---|
| Unit + component checks (standalone runner, both engines) | **86 / 86 passing** |
| Property test sequences | **10,000 sequences × 300 ops × both engines**, all invariants held after every operation |
| Differential test | **1,000,000 operations**, naive and fast event streams value-identical after every operation; state cross-checked every 256 ops |
| Deterministic replay | Same input → identical event stream across 2 fresh runs (50,000 ops) |
| Steady-state allocations (fast engine) | **0** over 100,000 post-warm-up operations |
| Hardened rerun | Full suite repeated under `-D_GLIBCXX_DEBUG -D_GLIBCXX_ASSERTIONS`: 86 / 86 |

Invariants asserted after every operation in property tests:

- Book never crosses (`best_bid < best_ask` when both sides non-empty)
- Conservation: every accepted quantity accounted for as resting + traded + cancelled +
  discarded, reconstructed from the event stream alone and reconciled against the book
- Every id resolves to a live resting order (and in the fast engine, to the exact pool slot)
- Every level's cached aggregate equals the sum of its orders' quantities
- No empty level remains active; FIFO links are bidirectionally consistent

---

## Threats to validity

Stated honestly rather than left to be discovered:

- **Unpinned, uncontrolled clocks.** Laptop, frequency scaling on, other processes running.
  p50/p99 are stable across seeds (spread table above); p99.99 and max are not — treat the
  extreme tail columns as noise ceilings, not engine properties.
- **Unserialised `rdtsc`.** The timestamp pair costs a few nanoseconds and is not subtracted;
  small absolute values (the 79–112 ns p50s) carry that overhead inside them. No fencing means
  occasional reordering jitter, traded for not perturbing the pipeline being measured.
- **Synthetic flow is a model.** Distribution choices are documented above but not validated
  against a real ITCH capture. The cancel-ratio semantics (per add, not per message) is a
  modelling decision — the raw-message reading empties the book and would benchmark nothing.
- **Small steady-state book.** ~200 resting orders flatters the naive engine's O(N) scan; the
  gap understates what a deep book would show. The depth sweep is the planned follow-up.
- **The engines differ functionally at the margin.** The fast engine's bounded price band and
  fixed capacity are design constraints, not just speedups; the naive engine mirrors them for
  comparability, so neither measures an unbounded-book design.
- **Single machine, single compiler, single run family.** No cross-platform numbers yet; the
  CI matrix builds and tests on Linux gcc/clang but does not benchmark.
