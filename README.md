# Limit Order Book & Matching Engine

[![build and test](https://github.com/sizzywizzy/Limit-Order-Book/actions/workflows/ci.yml/badge.svg)](https://github.com/sizzywizzy/Limit-Order-Book/actions/workflows/ci.yml)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/17)
[![CMake 3.16+](https://img.shields.io/badge/CMake-3.16%2B-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

A price-time priority matching engine for a single instrument, in C++17, built as two
interchangeable implementations behind one interface: a reference version on `std::map`, and an
optimised version using intrusive lists and a pre-allocated order pool.

The design goal is not fast matching. It is **O(1) cancellation** — because cancels, not trades,
are what an exchange actually processes.

> [!IMPORTANT]
> **Phase 0 of 4. There is no matching engine in this repository yet.**
> What exists is scaffolding that builds and passes its tests. Every performance claim is absent
> rather than estimated. See [Roadmap](#roadmap) for what lands when.

| | |
|---|---|
| **Working today** | Build system · test harness wired to CTest · CI with sanitizers · benchmark plotting pipeline · value types |
| **Not built** | Both engines · the benchmark harness · every performance number |
| **Updated** | August 2026 |

---

## Why this exists

Cancels and modifications dominate real exchange message flow — typically well over 90% of
messages never result in a trade. An engine tuned for fast matching but slow cancellation is
tuned for the rare case.

Every significant design decision here follows from that one fact. The intrusive list, the order
pool, and the direct id-to-pointer map all exist so that cancellation requires no search and no
deallocation. The reference engine exists so that the optimised one can be *proved* equivalent
rather than assumed correct.

---

## Results

> [!NOTE]
> **No performance figures yet.** The benchmark harness is phase 3 work, and the engines it would
> measure do not exist. Numbers appear here only once they come from a real run with a recorded
> environment and seed.

The reporting format is already fixed, so there is no opportunity to pick a flattering one after
seeing the data:

- nanoseconds per operation, **split by operation type** — cancel, passive add and aggressive add
  differ by roughly an order of magnitude, and aggregating them produces a number that describes
  nothing;
- reported as **p50 / p99 / p99.9 / p99.99 / max**, never means, because in latency-sensitive work
  the tail is the product;
- as the **median of five or more runs** with the spread stated, warm-up discarded, seed recorded.

Full methodology in [RESULTS.md](RESULTS.md#methodology).

---

## Quickstart

Requires CMake 3.16+ and a C++17 compiler. Verified on g++ 13.3 / Ubuntu 24.04; CI also runs
clang. Catch2 v3 is fetched at configure time, or used from the system if already installed.

```bash
git clone https://github.com/sizzywizzy/Limit-Order-Book.git && cd Limit-Order-Book
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

That is the whole build, and it works from a clean clone today.

> [!TIP]
> On Windows, build inside WSL2. `perf` and `cachegrind` are needed in phase 3 and the Linux
> tooling is materially better — setting this up now avoids discovering it in February.

<details>
<summary><b>Build options</b></summary>

| Option | Default | Purpose |
|---|---|---|
| `OB_BUILD_TESTS` | `ON` | Build the three test executables |
| `OB_BUILD_BENCH` | `ON` | Build `ob_bench` |
| `OB_WERROR` | `OFF` | Warnings as errors — CI sets this |
| `OB_NATIVE` | `OFF` | `-march=native`; **required for any benchmark run** |
| `OB_SANITIZE` | `OFF` | ASan + UBSan |

The sanitised build exists for one specific bug. Returning an order to the pool while the id map
still points at it is a use-after-free that typically surfaces only under load, so it is checked
mechanically rather than by reading the code:

```bash
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DOB_SANITIZE=ON
cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure
```

</details>

<details>
<summary><b>Benchmarks — phase 3</b></summary>

`ob_bench` currently prints its planned interface and exits non-zero. When it is implemented:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOB_NATIVE=ON && cmake --build build -j
./build/bench/ob_bench --engine=fast --orders=10000000 --cancel-ratio=0.9 \
                       --seed=1 --out=data/results.csv
python3 scripts/plot.py data/results.csv --outdir docs/figures
```

`scripts/plot.py` works now and is covered by CI. It reads `engine,operation,latency_ns` and
emits the percentile table as CSV and as markdown ready to paste into RESULTS.md, plus a
percentile bar chart and a tail CCDF. Every figure in the docs is regenerated from data by that
script — none are drawn by hand, so a stale plot is not possible.

</details>

---

## Repository layout

```
include/ob/
  types.hpp              Price, Quantity, OrderId, Side, OrderType    ✅
  order.hpp              the Order value type                         ✅
  book_naive.hpp         reference engine                             phase 1
  book_fast.hpp          optimised engine                             phase 2
  pool.hpp               pre-allocated order pool + free list         phase 2
  intrusive_list.hpp     intrusive FIFO                               phase 2
tests/
  test_basic.cpp         unit tests                                   phase 1
  test_properties.cpp    randomised invariant checks                  phase 1
  test_differential.cpp  naive vs fast, byte-for-byte                 phase 2
bench/
  generator.cpp          synthetic cancel-heavy order flow            phase 3
  bench_main.cpp         benchmark harness                            phase 3
scripts/plot.py          percentile tables and figures from CSV       ✅
```

| Document | Answers |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | What the system is — components, operations, complexity |
| [DECISIONS.md](DECISIONS.md) | Why each choice was made, and what was rejected |
| [RESULTS.md](RESULTS.md) | How performance is measured, and what was measured |

---

## Scope

What the engine is being built to do. Nothing here is implemented yet — this is the
specification, not a claim.

**Order types** — limit (passive and aggressive), market, immediate-or-cancel, fill-or-kill.

**Operations** — add, cancel, modify (in-place quantity reduction, or cancel-replace on price
change or quantity increase), partial fills across multiple price levels, self-trade prevention,
level-2 snapshot on demand, and a trade/book-update event stream.

**Guarantees the tests will have to enforce**

- Deterministic: identical input produces a byte-identical event stream, across runs and across
  both engines.
- Zero heap allocation in the steady state for the optimised engine after warm-up — proved with
  an allocation counter, not asserted.
- The book never crosses, checked after every operation rather than at the end of a run.

---

## Architecture

```
                 ┌──────────────┐
   commands  ──▶ │   Book       │ ──▶  trade / book-update events
                 │  (matching)  │
                 └──────┬───────┘
                        │
            ┌───────────┼───────────┐
            ▼           ▼           ▼
      ┌──────────┐ ┌─────────┐ ┌──────────┐
      │  Levels  │ │ Id map  │ │  Order   │
      │  (price  │ │ id →    │ │  pool    │
      │  indexed)│ │ Order*  │ │(freelist)│
      └────┬─────┘ └─────────┘ └──────────┘
           │
           ▼
     intrusive FIFO
     of Order* per level
```

| Component | Reference engine | Optimised engine |
|---|---|---|
| Price levels | `std::map<Price, Level>` | Array indexed by tick offset |
| FIFO at a level | `std::list<Order>` | Intrusive doubly-linked list |
| Id lookup | `std::unordered_map<OrderId, iterator>` | Flat map / direct index → `Order*` |
| Order storage | Per-order allocation | Pre-allocated pool with free list |
| Cancel | O(1) via stored iterator | O(1) via direct pointer unlink |

Both engines are O(1) on cancel, and that is the point worth understanding: the asymptotics are
nearly identical, so any measured difference comes from constant factors — allocation, pointer
chasing, cache behaviour. Which is precisely why phase 3 matters more than the complexity table.
Detail in [ARCHITECTURE.md](ARCHITECTURE.md).

---

## Testing

Three layers, each its own executable so a failure names its layer:

| Layer | Executable | What it establishes |
|---|---|---|
| Unit | `ob_test_basic` | Each order type and each edge case behaves as specified |
| Property | `ob_test_properties` | Five invariants hold after *every* operation over randomised sequences |
| Differential | `ob_test_differential` | The optimised engine's entire output stream is byte-identical to the reference engine's |

```bash
ctest --test-dir build --output-on-failure          # everything
ctest --test-dir build -R differential              # one layer
./build/tests/ob_test_basic "[types]"               # one tag
```

The differential layer is the one that matters. It is what licenses any claim that the optimised
engine is correct across input space, rather than on the cases someone thought to write down.

The edge cases the unit layer must cover: cancelling a non-existent order, an order that exactly
consumes a level, an order that consumes an entire side, zero quantity, duplicate ids, and a
self-trade. The invariants the property layer asserts are listed in
[tests/test_properties.cpp](tests/test_properties.cpp).

**Current state:** 5 unit tests passing, on the value types. The property and differential files
are specifications with no cases in them yet.

---

## Roadmap

Each phase has an entry condition and a definition of done. The failure mode this guards against
is optimising before correctness is airtight — which produces a fast engine that cannot be proved
right, and is therefore worth nothing.

| Phase | Contents | Definition of done |
|---|---|---|
| **0 · Foundations** *(current)* | Domain reading, environment, repo skeleton | Build and tests run from a clean clone; DECISIONS.md 001–002 written |
| **1 · Reference engine** | `std::map` + `std::list`, all order types, event stream | Unit tests pass; 10,000+ property sequences hold all five invariants; deterministic replay verified twice; baseline throughput recorded |
| **2 · Optimised engine** | Pool, intrusive lists, flat level array, direct id map | Passes every phase 1 test; differential test byte-identical over 1,000,000+ operations; allocation counter reads zero in steady state; cancel path contains no search |
| **3 · Measurement** | Flow generator, latency harness, profiling, optimisation passes | Full percentile tables with environment recorded; optimisation history including the changes that did not work |
| **4 · Presentation** | Results into this README, architecture diagram, decision log | A reviewer can understand the project in ninety seconds |

Phase 0 also owes DECISIONS.md entries 003–007: level storage, intrusive lists, pool sizing and
exhaustion policy, modify semantics, and the id lookup structure. The rule for this project is
that the entry is written *before* the code — if the justification cannot be articulated, the
implementation is not ready to be written.

---

## Limitations

Stated deliberately rather than left for a reader to find. Each is a scope decision, and each is
defensible.

- **Single instrument.** Multi-symbol support would need a symbol-indexed book map and would
  change cache behaviour materially.
- **Single threaded.** No concurrency control; the engine assumes exactly one writer. A matching
  engine core is conventionally single-threaded because the sequencing *is* the product —
  price-time priority requires a total order over events.
- **Bounded price range.** The optimised engine's level array covers `[base, base + N_TICKS)`.
  Orders outside it are rejected, explicitly and by assertion.
- **Fixed pool capacity.** Set at construction; exhaustion rejects new orders rather than growing.
- **No persistence or crash recovery.**

---

## What I would do next

To be written at the end of phase 3, when there is finished work to have opinions about.

---

## References

- NASDAQ TotalView-ITCH 5.0 specification
- Larry Harris, *Trading and Exchanges*, ch. 4–7
- Ulrich Drepper, *What Every Programmer Should Know About Memory*

## License

MIT — see [LICENSE](LICENSE).
