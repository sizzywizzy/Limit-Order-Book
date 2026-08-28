# Architecture

Detailed design of the matching engine. For *why* each choice was made, see
[DECISIONS.md](DECISIONS.md); this document describes *what* the system is.

---

## 1. Domain model

### The instrument

Each book trades exactly one **European option series** (`ob::OptionSeries`: underlying id,
expiry, strike, call/put). European exercise means nothing happens to the contract intraday
except trading, so the matching core carries the series as inert metadata and never reads it —
there are no exercise or assignment message paths to have bugs in. `ExerciseStyle` has exactly
one enumerator, so an American series is unrepresentable rather than unhandled
(DECISIONS.md 009). The book's price axis is the option *premium*, in ticks.

### The book

A limit order book holds resting orders on two sides. Bids are sorted descending by price, asks
ascending — so the "best" price on each side is the one an incoming order on the opposite side
meets first.

**Invariant.** At rest, `best_bid < best_ask` whenever both sides are non-empty. The book never
crosses. If an incoming order would cross, it matches immediately instead of resting; whatever
quantity remains unfilled rests at its limit price.

### Price-time priority

Two orders compete on two keys, in this order:

1. **Price.** A better price matches first — higher for bids, lower for asks.
2. **Time.** Among orders at the same price, the earliest arrival matches first.

This is why each price level is a **FIFO queue**, not a set or a heap. Position in that queue is
economically meaningful: being early means being filled first, which is sometimes valuable and
sometimes adverse.

### Order lifecycle

```
                    ┌─────────────┐
     add ─────────▶ │  crossing?  │
                    └──┬───────┬──┘
                   yes │       │ no
                       ▼       ▼
                  ┌────────┐  ┌────────┐
                  │ match  │  │  rest  │
                  └───┬────┘  └───┬────┘
                      │           │
            ┌─────────┴───┐       │
            ▼             ▼       ▼
        ┌────────┐  ┌──────────────────┐
        │ filled │  │  resting (in a   │
        └────────┘  │  level's FIFO)   │
                    └───┬─────────┬────┘
                        │         │
                 cancel │         │ matched by
                        ▼         ▼ an aggressor
                   ┌─────────┐ ┌────────┐
                   │cancelled│ │ filled │
                   └─────────┘ └────────┘
```

---

## 2. Types

| Type | Underlying | Notes |
|---|---|---|
| `Price` | `std::int64_t` | Ticks from a documented base. Never floating point. |
| `Quantity` | `std::uint64_t` | In lots. One lot is the instrument's minimum tradeable unit; never fractional. |
| `OrderId` | `std::uint64_t` | Client-assigned. **0 is reserved** (empty marker in the id index) and rejected. Uniqueness among live orders is enforced with a `DuplicateId` rejection. |
| `Side` | `enum class : uint8_t` | `Buy`, `Sell`. |
| `OrderType` | `enum class : uint8_t` | `Limit`, `Market`, `IOC`, `FOK`. |
| `OptionSeries` | struct | Underlying id, expiry (YYYYMMDD), strike, `OptionRight`, `ExerciseStyle::European`. |
| `LevelView` | struct | One row of a level-2 snapshot: price, aggregate qty, order count. |
| `Event` | struct | Fixed-layout value with defaulted `==`; see section 5. |
| `npos32` | `std::uint32_t` | "No slot" sentinel for all index-linked structures. |

Both engines share one public interface (identical signatures, identical event semantics):

```cpp
Book(base_price, num_ticks, capacity);            // + overload taking OptionSeries
Book(series, base_price, num_ticks, capacity);

void submit(id, side, type, price, qty);
void cancel(id);
void modify(id, new_price, new_qty);

std::optional<Price>    best_bid() / best_ask();
Quantity                quantity_at(side, price);
std::optional<Quantity> open_quantity(id);
std::size_t             order_count();
std::size_t             snapshot(side, std::span<LevelView>);   // best-first
const char*             validate();                             // nullptr = invariants hold
```

Engines are class templates over an **event sink** (`FastBook<Sink>`, `NaiveBook<Sink>`);
`NullSink` (default) compiles to nothing, `VectorSink` records for tests (DECISIONS.md 008).

---

## 3. Components

### 3.1 Level

One price point. Holds the FIFO of orders resting at that price plus a cached aggregate
quantity and order count.

The cached aggregate must be updated on **every** quantity change including partial fills and
in-place reductions, not just on add and remove. Drift here is a classic bug and is exactly
what the property-test invariant "cached aggregate equals sum of orders" is there to catch.

Empty levels must be deactivated. A level left active with zero orders will be found by a
best-price query and produce a wrong top of book — in the fast engine "active" *is* the
occupancy bit, so deactivation is `clear(tick)`.

### 3.2 Level container

**Reference engine.** One sorted `std::vector<NaiveLevel>` per side, searched with
`lower_bound`, spliced with `insert`/`erase`. O(P) worst-case maintenance, transparently
correct, no map (DECISIONS.md 010).

**Optimised engine.** A flat `std::vector<Level>` per side indexed by tick offset from the base
price — O(1) lookup, contiguous memory, no allocation — plus a **three-tier occupancy bitmap**
per side (`ob::TieredBitmap`): one bit per tick, one summary bit per leaf word, one top word.
Best bid = highest set bit, best ask = lowest set bit, next occupied level in either direction
= masked word scan; every query is at most three find-first-set instructions and three loads
(DECISIONS.md 003). Capacity: up to 64³ = 262,144 ticks per band.

The array imposes a bounded price range. Orders outside `[base, base + num_ticks)` are rejected
with `PriceOutOfRange`; this bound is a documented construction parameter, not an implicit
assumption.

### 3.3 Order storage

**Reference engine.** `std::list<Order>` owns its orders; each insertion allocates.

**Optimised engine.** A pre-allocated `SlotPool<FastOrder>` sized at construction, with a free
list threaded through the unused slots. `acquire()` pops the head, `release()` pushes onto it —
both O(1), neither allocates. Value-initialisation at construction touches every page, so the
pool is pre-faulted before the first order arrives.

```cpp
template <FreeListNode T>          // requires a std::uint32_t `next`
class SlotPool {
    std::vector<T> slots_;         // sized once, never resized
    std::uint32_t free_head_;
public:
    explicit SlotPool(std::size_t capacity);
    std::uint32_t acquire();       // npos32 when exhausted
    void          release(std::uint32_t);
    T&            operator[](std::uint32_t);
};
```

The free list reuses the `next` link already present in `FastOrder`, so it costs no additional
memory per slot. Exhaustion rejects new limit orders up front (`CapacityExhausted`) rather than
growing; IOC/FOK/market orders never need a slot and still match against a full book
(DECISIONS.md 005).

### 3.4 Intrusive FIFO

The links live **inside** `FastOrder`, as 32-bit slot indices rather than pointers
(DECISIONS.md 004):

```cpp
struct FastOrder {                 // 48 bytes
    OrderId       id;
    Price         price;
    Quantity      qty;             // remaining
    std::uint32_t prev, next;      // intrusive links; `next` doubles as free list
    std::uint32_t tick;            // owning level = price - base, cached
    Side          side;
};
```

Given a slot index, unlinking (`IndexFifo::unlink`) is four writes plus the level bookkeeping:

```
fifo.unlink(pool, i)               // 4 link writes, no search
level.agg   -= o.qty
level.count -= 1
if level.count == 0: bitmap.clear(o.tick)
index.erase(o.id)                  // BEFORE release — see 3.5
pool.release(i)
```

No search, no lookup, no deallocation. This is the entire justification for the structure: it
makes the most frequent operation the cheapest one.

### 3.5 Id index

Maps `OrderId → pool slot` so cancel and modify find their target in O(1).

**Reference engine.** Deliberately none: a linear scan of both sides *is* the specification of
what a lookup must return, and cannot have a stale-entry bug.

**Optimised engine.** `ob::IdIndex`: one flat power-of-two array, linear probing, splitmix64
finalizer hash, sized ≥ 2× pool capacity so load stays ≤ 0.5 and probe runs stay ~1 cache
line. Deletion is backward-shift compaction — no tombstones, so probe chains do not grow under
cancel-heavy flow (DECISIONS.md 007). Key 0 marks empty cells, which is why id 0 is rejected
at the boundary.

**Ordering hazard.** On any removal, erase from the id index *before* returning the slot to the
pool. The reverse order leaves a live index entry pointing at a recyclable slot — a
use-after-free that typically only manifests under load. This is the specific bug the sanitised
differential CI job exists to catch.

---

## 4. Operations

### 4.1 Submit

Validation, in a fixed order shared by both engines (each failure emits `Rejected` with the
matching reason and stops): id ≠ 0 → not a live duplicate → qty ≠ 0 → price in band (skipped
for market orders) → capacity (limit orders only) → FOK availability pre-pass.

```
if order crosses the opposite side:
    match against levels in price order, then time order within each level
    emit a Trade event per fill (price = maker's price)
    fully consumed makers: unlink, un-index, release
    exactly emptied levels: clear the occupancy bit
if quantity remains and the order type allows resting:
    acquire a slot, append to the level's FIFO tail, set the occupancy bit,
    insert into the id index, emit Accepted with the rested quantity
```

Order-type behaviour on residual quantity:

| Type | Residual after matching |
|---|---|
| Limit | Rests at its limit price (`Accepted` follows the trades) |
| Market | Discarded (never rests; matches to the edge of the band) |
| IOC | Discarded |
| FOK | Impossible — availability is established *before* matching; insufficient depth rejects the whole order (`FokUnfillable`) with no partial execution |

### 4.2 Cancel

```
look up slot by id             -> O(1)   (Rejected{UnknownOrder} if absent)
emit Cancelled with remaining
unlink from the level FIFO     -> O(1)
update level aggregate/count   -> O(1)
clear occupancy bit if empty   -> O(1)
erase from id index            -> O(1)   [must precede release]
release slot to pool           -> O(1)
```

This path has no loop and no search. That property is the design goal of the whole engine.

### 4.3 Modify

| Change | Behaviour | Queue position |
|---|---|---|
| Quantity decreased (same price) | Updated in place, `Reduced` emitted | **Preserved** |
| Quantity increased | Cancel-replace through the full submit pipeline | Lost — may trade, then rests at the tail |
| Price changed | Cancel-replace through the full submit pipeline | Lost — may trade, then rests at the tail |
| New quantity = 0 | Cancel | — |

Mirrors standard exchange behaviour: you may give up size without penalty, but you may not gain
priority you did not earn by waiting (DECISIONS.md 006).

---

## 5. Event output

Every operation emits a deterministic event sequence, which is what makes replay testing and
differential testing possible. Events are fixed-layout values with defaulted equality, pushed
into a compile-time sink — no virtual dispatch, no allocation (DECISIONS.md 008).

| Event | Emitted when | Payload notes |
|---|---|---|
| `Accepted` | An order (or its residual) comes to rest | qty = rested quantity |
| `Trade` | A match occurs | id = taker, maker_id = maker, price = maker's price, side = taker side |
| `Reduced` | In-place quantity reduction | qty = new remaining |
| `Cancelled` | A resting order leaves by request (cancel, modify-to-zero, or the cancel half of a replace) | qty = remaining at removal |
| `Rejected` | An operation is refused | `RejectReason`: InvalidId, DuplicateId, ZeroQuantity, PriceOutOfRange, CapacityExhausted, FokUnfillable, UnknownOrder |

There is deliberately no `BookUpdate` event: every aggregate change is derivable from the
stream above, and the property harness proves that continuously by reconstructing book state
from events alone.

**Determinism requirement.** The same input sequence must produce a value-identical event
stream across runs and across both engines. This is the property differential testing checks,
after every single operation.

---

## 6. Concurrency model

Single writer, single threaded. The engine performs no locking and assumes exactly one thread
mutates the book.

This is a deliberate scope decision, not an oversight: a matching engine core is conventionally
single-threaded precisely because the sequencing *is* the product — price-time priority
requires a total order over events, and concurrent mutation would destroy it.

Feeding the engine from another thread (as a market data feed handler would) is done with a
single-producer single-consumer queue outside the engine, not with locks inside it.

---

## 7. Complexity summary

| Operation | Reference | Optimised |
|---|---|---|
| Add, passive, existing level | O(log P) find + O(1) append | O(1) |
| Add, passive, new level | O(P) vector splice | O(1) |
| Add, aggressive | O(k) | O(k) |
| Cancel | O(N) scan | **O(1)** |
| Modify, quantity down | O(N) scan | **O(1)** |
| Modify, cancel-replace | O(N) + insert | O(1) |
| Best bid / ask | O(1) | O(1) — three word ops |
| Level-2 snapshot, d rows | O(d) | O(d) |

*P* = active price levels, *N* = resting orders, *k* = orders consumed by the aggressor.

The reference engine's O(N) cancel is the honest description of "no index at all"; the fast
engine's O(1) is by construction — the code path contains no loop. Both engines allocate
nothing per operation except the reference engine's list/vector nodes; the fast engine
allocates only at construction, a claim proved by a counting allocator in the property layer,
not asserted.
