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

**Decision.** A reference engine (`std::map` + `std::list`) and an optimised engine are both
maintained permanently behind the same public interface.

**Alternatives.** Build the optimised engine directly; build the naive one and delete it after.

**Reasoning.** The reference engine serves two roles that nothing else can. It is the
**correctness oracle** for differential testing — the only way to make a defensible claim that
the optimised engine is right across input space rather than just on hand-written cases. And it
is the **performance baseline** — speedup claims are meaningless without a stated, honest
comparison point, and a deliberately slow baseline is a form of cheating.

**Consequences.** Every feature must be implemented twice. This is real cost, and it is
accepted deliberately: it buys a validation strategy that no amount of unit testing replaces.

---

## 003 — <!-- FILL IN: level storage -->

<!-- Write this before implementing the level container in phase 2.
     Cover: why array-indexed-by-tick over std::map; what price range bound this imposes;
     what happens to an order outside the range; the memory cost of the array. -->

---

## 004 — <!-- FILL IN: intrusive lists for the FIFO -->

<!-- Cover: why the links live inside Order rather than in a container node; how this makes
     cancel O(1) with no lookup; the locality argument; what safety you give up
     (Order lifetime is now manual). -->

---

## 005 — <!-- FILL IN: order pool sizing and exhaustion policy -->

<!-- Cover: fixed capacity vs growth; what happens when the pool is exhausted (reject vs grow
     vs abort) and why; how the free list reuses the `next` pointer to cost no extra memory. -->

---

## 006 — <!-- FILL IN: modify semantics -->

<!-- Cover: quantity reduction keeps queue position; price change or quantity increase is a
     cancel-replace and loses it. Cite the exchange behaviour this mirrors. This is a very
     common interview probe. -->

---

## 007 — <!-- FILL IN: id lookup structure -->

<!-- Cover: unordered_map vs flat open-addressing vs direct array indexing; what assumption
     about id density each requires; the measured difference. -->

---

## Rejected ideas

Things considered and deliberately not done. Keeping this list is as useful as the decisions
themselves — it shows the design space was explored rather than stumbled into.

| Idea | Why not |
|---|---|
| <!-- e.g. Lock-free multi-writer book --> | <!-- Out of scope; single-writer is the realistic model for a matching engine core --> |
| | |

---

## Open questions

Things not yet resolved. Move them into a numbered decision once settled.

- [ ] <!-- e.g. Should the event stream be pushed to a callback or pulled from a queue? -->
- [ ] 
