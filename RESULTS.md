# Benchmark results

All latency figures are nanoseconds per operation unless stated otherwise.

**Methodology is specified before any numbers appear**, so the numbers can be judged against a
stated standard rather than taken on trust.

---

## Methodology

### Environment

<!-- FILL IN. Results without this section are not reproducible and a reviewer will discount
     them entirely. Get these from lscpu, `gcc --version`, and your CMake flags. -->

| | |
|---|---|
| CPU | _model, base/boost clock, physical cores_ |
| Caches | _L1d / L2 / L3 sizes_ |
| Memory | _size, type, speed_ |
| OS / kernel | _distribution, kernel version_ |
| Compiler | _g++ or clang++ version_ |
| Flags | `-O2 -march=native -DNDEBUG` <!-- state exactly what was used --> |
| Frequency scaling | _disabled / enabled — say which_ |
| Thread pinning | _pinned to core N / not pinned_ |
| Timer | `rdtsc` with documented calibration, or `steady_clock` — say which |

### Measurement rules

These are applied to every figure in this document:

1. **Percentiles, never means.** p50, p99, p99.9, p99.99, max. In latency-sensitive systems the
   tail is the product; a mean conceals exactly the behaviour being measured.
2. **Per operation type, separately.** Cancel, passive add, and aggressive add differ by
   roughly an order of magnitude. Aggregating them produces a number that describes nothing.
3. **Warm-up discarded.** The first _N_ operations are excluded so that cold caches, cold branch
   predictors and page faults do not contaminate steady-state figures.
4. **Median of ≥5 runs**, with the observed spread reported. A single run is an anecdote.
5. **Environment held constant** across every comparison in a given table.

### Workload

Synthetic order flow with a configurable mix. Defaults chosen to resemble real equity flow:

| Parameter | Value | Rationale |
|---|---|---|
| Cancel ratio | _0.90_ | Real order-to-trade ratios are heavily cancel-dominated |
| Aggressive fraction | _0.10_ | Most orders rest; a minority cross |
| Price distribution | Concentrated near touch, thinning outward | Uniform prices give an unrealistically flat book and flattering cache behaviour |
| Order size | Log-normal | Many small, few large — affects multi-level sweeps |
| Steady-state depth | _—_ levels | Depth changes level-scan cost |
| Total operations | _—_ | After warm-up |

Seed is fixed and recorded so every run is reproducible: `seed = _____`.

---

## Headline comparison

<!-- Phase 3. Do not type this table by hand: run
         python3 scripts/plot.py data/results.csv --outdir docs/figures
     and paste docs/figures/latency_percentiles.md over the table below. The
     same table goes into the README. -->


| Operation | Engine | p50 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| Cancel | reference | | | | | |
| Cancel | optimised | | | | | |
| Add (passive) | reference | | | | | |
| Add (passive) | optimised | | | | | |
| Add (aggressive) | reference | | | | | |
| Add (aggressive) | optimised | | | | | |
| Modify (qty down) | reference | | | | | |
| Modify (qty down) | optimised | | | | | |

**Throughput**

| Engine | Operations/second |
|---|---|
| Reference | |
| Optimised | |

---

## Optimisation history

One row per change. **Record the failures too** — a write-up containing "I tried this, it made
no measurable difference, and here is why I think that is" reads as considerably more credible
engineering than an unbroken list of wins.

| # | Change | Hypothesis | Cancel p99 before | after | Δ | Kept? |
|---|---|---|---|---|---|---|
| 1 | <!-- e.g. Replace std::unordered_map with flat open-addressing map --> | <!-- Reduces pointer chasing on the cancel lookup --> | | | | |
| 2 | <!-- e.g. Pack hot Order fields into one cache line --> | | | | | |
| 3 | <!-- e.g. Pre-fault the order pool at construction --> | | | | | |
| 4 | | | | | | |

### Notes on individual changes

#### 1. <!-- Change name -->

<!-- What you profiled that led to this hypothesis, what you changed, what the numbers did, and
     your interpretation. Three or four sentences. If the result surprised you, say so and say
     what you think explains it — that is the most interesting paragraph in this document. -->

---

## Profiling evidence

### `perf stat` summary

<!-- Paste the output for both engines on the same workload. Cycles, instructions, IPC,
     cache-misses, branch-misses. -->

```
# reference engine

# optimised engine

```

**Interpretation.** <!-- What the counters say about where the difference actually comes from.
     Instruction count, or cache misses, or branch prediction? Be specific. -->

### Allocation count in the steady state

<!-- The claim "zero allocation in the hot path" must be proved, not asserted. State how you
     counted (overridden operator new, a counting allocator, or ltrace) and give the number. -->

| Engine | Allocations during steady-state run |
|---|---|
| Reference | |
| Optimised | _expected: 0_ |

---

## Scaling behaviour

### Latency vs book depth

<!-- How does each engine degrade as the number of active price levels grows? The reference
     engine's O(log P) should show; the optimised engine's O(1) should be flat. This plot is
     one of the more persuasive artifacts in the project. -->

| Depth (levels) | Reference cancel p99 | Optimised cancel p99 |
|---|---|---|
| 10 | | |
| 100 | | |
| 1,000 | | |
| 10,000 | | |

### Latency vs cancel ratio

<!-- Sweep the cancel ratio from 0.5 to 0.99. The gap between engines should widen as cancels
     dominate — which is the empirical demonstration of the project's central design argument. -->

| Cancel ratio | Reference throughput | Optimised throughput |
|---|---|---|
| 0.50 | | |
| 0.75 | | |
| 0.90 | | |
| 0.99 | | |

---

## Correctness evidence

Performance figures mean nothing without this section.

| Check | Result |
|---|---|
| Unit tests | _—_ / _—_ passing |
| Property test sequences | _—_ randomised sequences, all invariants held |
| Differential test | _—_ operations, output streams byte-identical |
| Deterministic replay | Same input → identical output across _—_ runs |

Invariants asserted after every operation in property tests:

- Book never crosses (`best_bid < best_ask` when both sides non-empty)
- Quantity conservation: added = resting + traded + cancelled
- Every id in the map resolves to a live order in a level
- Every level's cached aggregate equals the sum of its orders' quantities
- No empty level remains active

---

## Threats to validity

Stated honestly rather than left to be discovered:

- <!-- e.g. Synthetic flow is a model of real flow, not real flow. Distribution choices affect
     results and are documented above but not validated against a real ITCH capture. -->
- <!-- e.g. Single machine, single configuration. No cross-platform comparison. -->
- <!-- e.g. rdtsc measurement overhead is ~N ns and is not subtracted from the figures. -->
- <!-- e.g. The optimised engine assumes a bounded price range; the reference engine does not.
     This is a functional difference, not purely a performance one. -->
