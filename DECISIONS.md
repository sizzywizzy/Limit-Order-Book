# Design decisions

Every non-obvious choice in this codebase, with the alternative that was rejected and why.

**Rule: write the entry before writing the code.** If the justification cannot be articulated,
the implementation is not ready to be written. This file doubles as interview preparation —
every entry here is a question someone will ask.

Format for each entry:

> **Decision.** What was chosen.
> **Alternatives.** What else was considered.
> **Reasoning.** Why this one won.
> **Consequences.** What this makes easy, and what it makes hard or impossible.

---

## 001 — Prices are integers, not floating point

**Decision.** `Price` is `std::int64_t`, measured in ticks from a documented base.

**Alternatives.** `double`; a fixed-point wrapper class; `std::decimal`.

**Reasoning.** Prices are inherently discrete — instruments trade in ticks, not on a continuum.
Price equality comparison happens on essentially every operation, and floating point makes
equality ambiguous: two prices that should be identical can differ in the last bit after
arithmetic. An integer comparison is also a single instruction, where a float comparison plus
epsilon tolerance is several.

**Consequences.** Conversion to and from display prices happens at the boundary and must be
done in exactly one place. The tick size becomes an explicit, documented parameter rather than
an implicit assumption. Arithmetic on prices cannot silently lose precision.

---

## 002 — Two engines, one interface

**Decision.** A reference engine and an optimised engine are both maintained permanently behind
the same public interface, emitting the same event stream.

**Alternatives.** Build the optimised engine directly; build the naive one and delete it after.

**Reasoning.** The reference engine serves two roles that nothing else can. It is the
**correctness oracle** for differential testing — the only way to make a defensible claim that
the optimised engine is right across input space rather than just on hand-written cases. And it
is the **performance baseline** — speedup claims are meaningless without a stated, honest
comparison point, and a deliberately slow baseline is a form of cheating.

**Consequences.** Every feature must be implemented twice. This is real cost, and it is
accepted deliberately: it buys a validation strategy that no amount of unit testing replaces.
As of the current tree, the differential layer has held both engines value-identical over
1,000,000+ randomised operations per run.

---

## 003 — Price levels in a flat array, best prices in a tiered bitmap

**Decision.** Each side's levels live in one contiguous `std::vector<Level>` indexed by tick
offset from a base price, so level lookup is one add and one array index. Which levels are
occupied is tracked in a three-tier occupancy bitmap (one bit per tick, one summary bit per
64-bit leaf word, one top word over the summaries — up to 64³ = 262,144 ticks). Best bid is
"highest set bit", best ask is "lowest set bit": at most three find-first-set instructions and
three loads, with no incremental cursor state to keep coherent.

**Alternatives.**
- `std::map<Price, Level>` — the classic. O(log P), a red-black node allocation per level, and
  every lookup is a pointer chase through nodes scattered across the heap.
- A min-max heap (or `std::priority_queue` pair) over active prices — O(1) peek at the best,
  but O(log P) insert and delete, and critically **no O(1) removal of an arbitrary level**:
  when a cancel empties a level that is not the best, a heap needs an auxiliary index plus a
  sift to fix up, and its comparison chain is a run of unpredictable branches.
- Flat array with incrementally maintained `best_bid`/`best_ask` cursors and a linear walk to
  the next occupied level when the best empties — simple, and usually fine, but the walk is
  O(gap) with a worst case of the whole band, and "usually fine" is exactly the kind of tail
  the percentile reporting exists to expose.

**Reasoning.** The book does three price-structure operations constantly: find a level by
price, find the best level, and activate/deactivate a level. The array makes the first O(1).
The bitmap makes the other two O(1) — not amortised, not expected — with word operations on a
structure small enough to stay resident (a 16,384-tick side is 2 KiB of leaf bits plus 40 bytes
of summaries). Branches are near-absent; `countl_zero`/`countr_zero` compile to single
instructions. Against the map this removes allocation and pointer chasing; against the heap it
removes the log factor, the sift-up branches, and the arbitrary-deletion problem, because
clearing any bit costs the same as clearing the best one.

**Consequences.** The price range is bounded: orders outside `[base, base + num_ticks)` are
rejected explicitly, and the band is a construction parameter documented per instrument. Memory
is O(num_ticks) per side regardless of occupancy (24 bytes per level slot), which is the price
of O(1) indexing and is a few hundred KiB for realistic bands. The 64³ tier cap is asserted at
construction; a wider band needs a fourth tier, which is a mechanical extension.

---

## 004 — Intrusive FIFO with 32-bit slot indices, not pointers

**Decision.** The `prev`/`next` links of each level's FIFO live inside `FastOrder` itself, as
`std::uint32_t` indices into the order pool, with `npos32` as the null. Unlinking is four
writes given a slot index. `Level` stores only `{head, tail}` plus its aggregates.

**Alternatives.** `std::list<Order>` per level (a node allocation per order, cancel needs a
stored iterator); intrusive links as raw `Order*` pointers (the classic HFT layout, and what
the build manual sketches); `boost::intrusive::list`.

**Reasoning.** Intrusive-ness is the non-negotiable part: it is what makes cancel O(1) with no
search and no deallocation, and cancel volume is why this project exists. Indices rather than
pointers is the refinement: they are half the size (8 bytes of links per order instead of 16,
so `FastOrder` is 48 bytes — under one cache line), they stay valid under anything that moves
the pool's storage, they serialise trivially, and the pool's free list can reuse the same
`next` field with no casting games. The index arithmetic (`pool[i]`) costs one add against a
base register that the optimiser keeps hot.

**Consequences.** Order lifetime is manual: a slot's contents are dead the instant it returns
to the pool, and nothing enforces that except discipline and the ordering rule in 007/3.5 —
which is precisely what the sanitised differential run exists to check mechanically. A book is
limited to 2³²−2 concurrent orders, which is not a real limit for a per-instrument book.

---

## 005 — Fixed-capacity pool; exhaustion rejects; free list through `next`

**Decision.** `SlotPool<FastOrder>` is sized once at construction, value-initialised (which
pre-faults every page), and threads its free list through the `next` link the order already
carries. A limit order that would need a slot when `in_use == capacity` is rejected up front
with `CapacityExhausted`, before any matching. IOC, FOK and market orders are exempt — they
never rest, so they still match against a full book.

**Alternatives.** Grow on exhaustion (amortised allocation); abort on exhaustion; reject only
the residual after matching (fill what crosses, then drop what would have rested).

**Reasoning.** Growth reintroduces allocation on exactly the path the pool exists to clean, and
does it at the worst possible moment — peak load. Aborting turns an operational condition into
an outage. Rejecting the residual after matching was the closest call: it is more permissive,
but it makes acceptance depend on fill outcome, so the same order in the same book state either
trades-then-vanishes or rests depending on quantity arithmetic — a semantics that is harder to
state, harder to test, and surprising to a client. Up-front rejection keeps "accepted" meaning
one thing, and a full book is an operational fault that should be loud. The free-list reuse
costs zero extra bytes per slot; value-initialisation makes first-touch page faults a
construction cost instead of a first-thousand-operations cost.

**Consequences.** Capacity is a per-book configuration decision that must be sized to the
instrument (it is also what bounds the id index's load factor, see 007). A fully-booked venue
refuses new passive interest rather than degrading — the failure mode is visible in the event
stream as explicit rejections.

---

## 006 — Modify: reduce in place, otherwise cancel-replace

**Decision.** `modify(id, new_price, new_qty)` with the same price and `new_qty ≤` current
updates in place and **keeps queue position**, emitting `Reduced`. A price change or a quantity
increase is executed as cancel + fresh submit of a limit order with the same id — it re-enters
the pipeline (it may trade immediately, rest at the tail, or be rejected by the band check) and
**loses queue position**, emitting `Cancelled` then whatever the submit produces. Modify to
quantity zero is a cancel.

**Alternatives.** Keep priority on any same-price modify (including increases); a dedicated
"replace" message with its own id pair; rejecting price changes outright.

**Reasoning.** This mirrors standard exchange behaviour (NASDAQ's Order Replace and NSE's
modification rules have the same shape): you may give up size without penalty, because that
takes nothing from anyone behind you; you may not gain size or move price while keeping the
time priority you earned under different terms — that would let a front-of-queue order be
silently rewritten into a different economic bet. Routing the replace through the ordinary
submit path means there is exactly one matching pipeline to test, and the differential layer
holds both engines to it.

**Consequences.** A modify can produce a multi-event sequence (`Cancelled`, trades, `Accepted`)
that clients must be prepared to read. A replace to an out-of-band price kills the order rather
than leaving the old one resting — defensible (a cancel-replace is two operations, and the
cancel half succeeded) and stated here deliberately.

---

## 007 — Id lookup: flat open addressing with backward-shift deletion

**Decision.** `IdIndex` maps client-assigned 64-bit ids to pool slots in one contiguous
power-of-two table, linear probing, splitmix64-finalizer hash, sized to at least twice the pool
capacity so load never exceeds 0.5. Deletion is backward-shift compaction, not tombstones.
Id 0 is reserved as the empty-cell marker and rejected at the boundary (`InvalidId`).

**Alternatives.** `std::unordered_map<OrderId, u32>` — a node allocation per insert, a pointer
chase per lookup, and bucket iteration that wanders the heap. Direct array indexing by id — the
fastest possible, but requires the engine to assign dense ids, and this book accepts client
ids, which are arbitrary. Tombstone deletion — simpler to write, but under cancel-dominated
flow the tombstone count grows monotonically between rehashes, probe chains lengthen, and the
structure needs periodic rebuilding: a latency cliff scheduled for later.

**Reasoning.** Cancel's first step is this lookup, so it sits on the hottest path in the
engine. At load ≤ 0.5 with a well-mixed hash, the expected probe run is ~1.5 cells — one cache
line — and there is no allocation, no rehash, and no degradation over time: backward-shift
leaves the table exactly as if the erased key had never been inserted. The mixing step matters:
client ids are typically sequential, and without a finalizer they would stride through the
table in runs.

**Consequences.** The table is 16 bytes × 2 × capacity of committed memory (2 MiB at 64k
orders) — bought once at construction. The ordering hazard of ARCHITECTURE.md 3.5 lives here:
erase from this index *before* releasing the slot, or a recycled slot is reachable under a dead
id. Reserving id 0 is an API restriction, documented and enforced with an explicit rejection.

---

## 008 — Events through a compile-time sink; fixed-layout comparable values

**Decision.** Engines are templates over an `EventSink` concept and emit fixed-size `Event`
values (`Accepted`, `Trade`, `Reduced`, `Cancelled`, `Rejected`) with defaulted equality. The
default sink is a no-op that vanishes under optimisation; tests use a recording sink;
`BookUpdate` is not emitted at all.

**Alternatives.** Virtual observer interface; a returned `std::vector<Event>` per call; an
internal ring buffer the caller drains; a `BookUpdate` event per aggregate change.

**Reasoning.** A virtual call per event is an indirect branch on the hot path and blocks
inlining; returning vectors allocates. The compile-time sink costs nothing when unused and
exactly the sink's own code when used, and it makes determinism testable: two engines, same
input, `operator==` over whole streams. `BookUpdate` was dropped because it is derivable — every
aggregate change is implied by an `Accepted`/`Trade`/`Reduced`/`Cancelled` already in the
stream — and emitting it would double event volume to say nothing new. The ledger in the
property harness reconstructs book state from the stream, which proves the derivability claim
continuously.

**Consequences.** The sink type is part of the book's type (`FastBook<VectorSink>`), so
production and test instantiations are distinct types — acceptable in exchange for zero
dispatch cost. Consumers wanting level deltas compute them from the stream.

---

## 009 — The instrument is a European option series, held as inert metadata

**Decision.** `OptionSeries` (underlying id, expiry, strike, call/put) is carried by the book
for identification, and `ExerciseStyle` has exactly one enumerator: `European`. The matching
core never reads any of it.

**Alternatives.** Supporting both exercise styles behind a flag; modelling the instrument
inside the engine (expiry checks, exercise messages); leaving instruments out entirely.

**Reasoning.** The scope is European options, and European exercise means nothing can happen to
the contract intraday except trading — no early exercise, no assignment risk touching resting
orders mid-session. That is why the matching core can be instrument-agnostic *by proof rather
than by hope*: there is no intraday lifecycle event to handle, so none is handled, and making
American style unrepresentable in the type turns "we don't support early exercise" from a
runtime promise into a compile-time fact. Settlement at expiry is a clearing-house process,
outside a matching engine's boundary.

**Consequences.** One book per series; a venue quoting a chain instantiates many books (they
are cheap: the arrays dominate, and construction is the only allocation). Adding American
options later is deliberately *not* a flag flip — it would add an enumerator and force every
consumer of `ExerciseStyle` to be revisited, which is the correct amount of friction for a
semantics change.

---

## 010 — The reference engine is map-free too: sorted vectors and linear scans

**Decision.** `NaiveBook` keeps each side as a `std::vector` of levels sorted by price
(`lower_bound` + `insert`/`erase`), `std::list<Order>` FIFOs, and **no id index at all** —
cancel and modify find their order by scanning the book. This makes the codebase contain no
`std::map` and no `std::unordered_map` anywhere in engine code.

**Alternatives.** The classic `std::map<Price, Level>` + `std::unordered_map<OrderId, iterator>`
reference (what the build manual sketches).

**Reasoning.** Two forces. First, the project constraint: no maps in the order book — and the
reference engine is an order book, so it honours the same constraint rather than smuggling the
banned structure in through the test fixture. Second, and independently: the oracle's one job
is to be *obviously* correct. A sorted vector inspected with `lower_bound` is a transparent
model of "levels in price order"; a linear scan is a transparent model of "the order with this
id, wherever it is" — it cannot have a stale-index bug, because there is no index to go stale.
The oracle got simpler, not just compliant. O(P) level insertion and O(N) cancel are fine in a
component whose job description is trust, not speed — and it still gives the benchmark a
baseline whose constant factors resemble the map version's story (node-per-order allocation,
pointer-chasing FIFOs).

**Consequences.** Differential runs are bounded by the naive engine's O(N) scans, so their op
counts are chosen with the oracle's speed in mind (1M ops ≈ tens of seconds at test
capacities). The baseline is *slower* per cancel than a `std::map` reference would be at large
books, which would flatter the speedup ratio — RESULTS.md therefore reports absolute numbers
for both engines rather than leading with a ratio.

---

## Rejected ideas

Things considered and deliberately not done. Keeping this list is as useful as the decisions
themselves — it shows the design space was explored rather than stumbled into.

| Idea | Why not |
|---|---|
| Lock-free multi-writer book | Out of scope; single-writer is the realistic model for a matching engine core — sequencing *is* the product (ARCHITECTURE.md 6) |
| Min-max heap over active price levels | O(log P) with branchy sifts and no O(1) arbitrary-level deletion; the tiered bitmap does the same job in O(1) word ops (see 003) |
| `std::pmr` arenas instead of the slot pool | Still allocator-shaped: per-object headers, type erasure through `memory_resource`, and no free-list-through-`next` trick; the pool is smaller and simpler |
| Pointer-based intrusive links | Twice the link footprint of 32-bit indices for no capability the book uses (see 004) |
| Tombstone deletion in the id index | Probe chains grow monotonically under cancel-heavy flow — a scheduled latency cliff (see 007) |
| Per-order `shared_ptr`/`unique_ptr` ownership | Ownership is already exactly the pool's; smart pointers would add control blocks and destructor plumbing to a structure whose lifetime discipline is the design |
| `BookUpdate` events | Derivable from the rest of the stream; doubling event volume to restate it buys nothing (see 008) |
| Self-trade prevention in v1 | Needs an owner/account field and a policy matrix (cancel-newest/oldest/both); scoped out until the Order type grows an owner — listed in README limitations |

---

## Open questions

Things not yet resolved. Move them into a numbered decision once settled.

- [ ] Should aggressive-order fills also appear as a terminal "done" event for the taker
      (fully-filled IOC/market orders currently end silently after their trades)?
- [ ] Snapshot delivery for late joiners: the L2 `snapshot()` query exists, but a replayable
      snapshot+delta protocol (sequence numbers in events) is unspecified.
- [ ] Whether the bitmap should grow a fourth tier (→ 16.7M ticks) or stay capped with the
      band a hard product constraint.
