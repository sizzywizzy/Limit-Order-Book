# Architecture

Detailed design of the matching engine. For *why* each choice was made, see
[DECISIONS.md](DECISIONS.md); this document describes *what* the system is.

---

## 1. Domain model

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
| `OrderId` | `std::uint64_t` | Client-assigned. Uniqueness is the caller's responsibility. |
| `Side` | `enum class : uint8_t` | `Buy`, `Sell`. |
| `OrderType` | `enum class : uint8_t` | `Limit`, `Market`, `IOC`, `FOK`. |

---

## 3. Components

### 3.1 Level

One price point. Holds the FIFO of orders resting at that price plus a cached aggregate
quantity.

The cached aggregate must be updated on **every** quantity change including partial fills, not
just on add and remove. Drift here is a classic bug and is exactly what the property test on
"cached aggregate equals sum of orders" is there to catch.

Empty levels must be removed or marked inactive. A level left in place with zero orders will be
found by a best-price scan and produce a wrong top of book.

### 3.2 Level container

**Reference engine.** `std::map<Price, Level>`, with a reversed comparator for the bid side.
O(log P) lookup, allocates a red-black tree node per level, and levels are scattered in memory.

**Optimised engine.** A flat array indexed by tick offset from a base price, plus incrementally
maintained `best_bid` / `best_ask` cursors. O(1) lookup, contiguous memory, no allocation.

The array imposes a bounded price range. Orders outside `[base, base + N_TICKS)` are rejected;
this bound is documented and asserted rather than left implicit.

### 3.3 Order storage

**Reference engine.** `std::list<Order>` owns its orders; each insertion allocates.

**Optimised engine.** A pre-allocated `std::vector<Order>` sized at construction, with a free
list threaded through the unused slots. `acquire()` pops the head, `release()` pushes onto it —
both O(1), neither allocates.

The free list reuses the `next` pointer already present in `Order`, so it costs no additional
memory per slot.

```cpp
class OrderPool {
    std::vector<Order> storage_;   // sized once, never resized
    Order* free_head_;
public:
    explicit OrderPool(std::size_t capacity);
    Order* acquire();              // nullptr when exhausted
    void   release(Order* o);
};
```

### 3.4 Intrusive FIFO

The `prev` and `next` pointers live **inside** `Order`, not in a wrapper node:

```cpp
struct Order {
    OrderId  id;
    Price    price;
    Quantity qty;
    Side     side;
    Order*   prev;    // intrusive links
    Order*   next;
    Level*   level;   // owning level, for O(1) bookkeeping on unlink
};
```

Given an `Order*`, unlinking is:

```
o->prev->next = o->next
o->next->prev = o->prev
o->level->total_qty -= o->qty
pool.release(o)
```

No search, no lookup, no deallocation. This is the entire justification for the structure: it
makes the most frequent operation the cheapest one.

### 3.5 Id map

Maps `OrderId → Order*` so cancel and modify can find their target in O(1).

**Ordering hazard.** On cancel, erase from the id map *before* returning the order to the pool.
The reverse order leaves a live map entry pointing at a recycled slot — a use-after-free that
typically only manifests under load.

---

## 4. Operations

### 4.1 Add

```
if order crosses the opposite side:
    match against levels in price order, then time order within each level
    emit a trade event per fill
    update or remove each fully-consumed resting order
if quantity remains and the order type allows resting:
    acquire an Order from the pool
    append to the tail of its price level's FIFO
    insert into the id map
    update best_bid / best_ask if this is a new top of book
```

Order-type behaviour on residual quantity:

| Type | Residual after matching |
|---|---|
| Limit | Rests at its limit price |
| Market | Discarded (never rests) |
| IOC | Discarded |
| FOK | Entire order is rejected unless it can fill completely — check *before* matching |

FOK requires a pre-pass to determine total available quantity at acceptable prices, because it
must not partially execute.

### 4.2 Cancel

```
look up Order* by id           -> O(1)
unlink from its level's FIFO   -> O(1)
decrement level aggregate      -> O(1)
erase from id map              -> O(1)   [must precede release]
release to pool                -> O(1)
if the level is now empty: mark inactive, advance best_bid/best_ask if needed
```

This path has no loop and no search. That property is the design goal of the whole engine.

### 4.3 Modify

| Change | Behaviour | Queue position |
|---|---|---|
| Quantity decreased | Updated in place | **Preserved** |
| Quantity increased | Cancel-replace | Lost — moves to tail |
| Price changed | Cancel-replace | Lost — moves to tail |

Mirrors standard exchange behaviour: you may give up size without penalty, but you may not gain
priority you did not earn by waiting.

---

## 5. Event output

Every operation emits a deterministic event sequence, which is what makes replay testing and
differential testing possible.

| Event | Emitted when |
|---|---|
| `Trade` | A match occurs. Carries aggressor id, resting id, price, quantity. |
| `Accepted` | An order rests. |
| `Cancelled` | An order is removed by request. |
| `Rejected` | An order is refused (FOK unfillable, price out of range, pool exhausted). |
| `BookUpdate` | A level's aggregate quantity changes. |

**Determinism requirement.** The same input sequence must produce a byte-identical event stream
across runs and across both engines. This is the property differential testing checks.

---

## 6. Concurrency model

Single writer, single threaded. The engine performs no locking and assumes exactly one thread
mutates the book.

This is a deliberate scope decision, not an oversight: a matching engine core is conventionally
single-threaded precisely because the sequencing *is* the product — price-time priority requires
a total order over events, and concurrent mutation would destroy it.

Feeding the engine from another thread (as the market data feed handler project does) is done
with a single-producer single-consumer queue outside the engine, not with locks inside it.

---

## 7. Complexity summary

| Operation | Reference | Optimised |
|---|---|---|
| Add, passive, existing level | O(log P) | O(1) |
| Add, passive, new level | O(log P) | O(1) |
| Add, aggressive | O(log P + k) | O(k) |
| Cancel | O(1)* | O(1) |
| Modify, quantity down | O(1)* | O(1) |
| Modify, cancel-replace | O(log P) | O(1) |
| Best bid / ask | O(1) | O(1) |

*P* = number of distinct price levels, *k* = number of resting orders consumed.
\* Reference cancel is O(1) only because the iterator is stored in the id map; without that it
degrades to a linear scan of the level.

Note that asymptotic complexity is nearly identical between the two engines for the important
operations. **The measured difference comes almost entirely from constant factors** — allocation,
pointer chasing and cache behaviour — which is precisely why the benchmark work in phase 3
matters more than the complexity table.
