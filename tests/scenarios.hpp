#pragma once

// The unit scenario set, engine-generic. One definition, three consumers:
// the Catch2 basic layer runs it per engine with REQUIRE, the standalone
// runner runs it with its own reporter, and the differential layer gets it
// for free because every scenario here is also reachable by the generator.
//
// Each scenario asserts on the emitted event stream — not just final state —
// because two engines can agree on where they end up and disagree about how
// they got there.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "harness.hpp"
#include "ob/book_fast.hpp"
#include "ob/book_naive.hpp"
#include "ob/events.hpp"
#include "ob/instrument.hpp"
#include "ob/types.hpp"

namespace obtest {

template <typename Book>
struct Fixture {
    Book book;
    explicit Fixture(std::size_t capacity = 64)
        : book(100, 64, capacity) {}  // prices 100..163

    std::vector<ob::Event> take() {
        std::vector<ob::Event> evs = std::move(book.sink().events);
        book.sink().events.clear();
        return evs;
    }
};

inline std::string diff_events(const std::vector<ob::Event>& got,
                               const std::vector<ob::Event>& want) {
    if (got == want) {
        return {};
    }
    std::string s = "got " + std::to_string(got.size()) + " events, want " +
                    std::to_string(want.size());
    const std::size_t n = got.size() < want.size() ? got.size() : want.size();
    for (std::size_t i = 0; i <= n; ++i) {
        const std::string g = i < got.size() ? to_string(got[i]) : "<none>";
        const std::string w = i < want.size() ? to_string(want[i]) : "<none>";
        if (i == n || g != w) {
            s += "; event " + std::to_string(i) + ": got " + g + ", want " + w;
            break;
        }
    }
    return s;
}

// `check(ok, description)` is called once per assertion.
template <typename Book, typename CheckFn>
void unit_scenarios(CheckFn&& check, const char* engine) {
    using ob::Event;
    using ob::OrderType;
    using ob::RejectReason;
    using ob::Side;

    const auto name = [engine](const char* what) {
        return std::string(engine) + ": " + what;
    };
    const auto check_events = [&](Fixture<Book>& f,
                                  const std::vector<Event>& want,
                                  const char* what) {
        const std::string err = diff_events(f.take(), want);
        check(err.empty(), name(what) + (err.empty() ? "" : (": " + err)));
    };

    {  // empty book
        Fixture<Book> f;
        check(!f.book.best_bid() && !f.book.best_ask() &&
                  f.book.order_count() == 0,
              name("empty book has no best prices"));
    }
    {  // passive add rests and is queryable
        Fixture<Book> f;
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 5);
        check_events(f, {Event::accepted(1, Side::Buy, 110, 5)},
                     "passive add emits Accepted");
        check(f.book.best_bid() == std::optional<ob::Price>(110) &&
                  f.book.quantity_at(Side::Buy, 110) == 5 &&
                  f.book.open_quantity(1) == std::optional<ob::Quantity>(5),
              name("passive add is queryable"));
    }
    {  // FIFO priority within a level
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 4);
        f.book.submit(2, Side::Sell, OrderType::Limit, 120, 4);
        f.take();
        f.book.submit(3, Side::Buy, OrderType::Limit, 120, 6);
        check_events(f,
                     {Event::trade(3, 1, Side::Buy, 120, 4),
                      Event::trade(3, 2, Side::Buy, 120, 2)},
                     "same-price makers fill in arrival order");
        check(f.book.open_quantity(2) == std::optional<ob::Quantity>(2),
              name("second maker keeps its residual"));
    }
    {  // price priority across levels, and Accepted comes after trades
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 125, 3);
        f.book.submit(2, Side::Sell, OrderType::Limit, 123, 3);
        f.take();
        f.book.submit(3, Side::Buy, OrderType::Limit, 125, 8);
        check_events(f,
                     {Event::trade(3, 2, Side::Buy, 123, 3),
                      Event::trade(3, 1, Side::Buy, 125, 3),
                      Event::accepted(3, Side::Buy, 125, 2)},
                     "better-priced maker fills first; residual rests last");
        check(f.book.best_bid() == std::optional<ob::Price>(125) &&
                  !f.book.best_ask(),
              name("aggressor residual becomes the new best bid"));
    }
    {  // exact level consumption removes the level
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 6);
        f.book.submit(2, Side::Sell, OrderType::Limit, 121, 5);
        f.take();
        f.book.submit(3, Side::Buy, OrderType::Limit, 120, 6);
        f.take();
        check(f.book.best_ask() == std::optional<ob::Price>(121) &&
                  f.book.quantity_at(Side::Sell, 120) == 0,
              name("exactly consumed level disappears from best price"));
    }
    {  // market order sweeps and its residual is discarded
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 2);
        f.book.submit(2, Side::Sell, OrderType::Limit, 121, 2);
        f.take();
        f.book.submit(3, Side::Buy, OrderType::Market, 0, 10);
        check_events(f,
                     {Event::trade(3, 1, Side::Buy, 120, 2),
                      Event::trade(3, 2, Side::Buy, 121, 2)},
                     "market order sweeps the side");
        check(f.book.order_count() == 0 && !f.book.open_quantity(3),
              name("market residual never rests"));
    }
    {  // market order into an empty side does nothing
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Market, 0, 5);
        check(f.take().empty(), name("market into empty book emits nothing"));
    }
    {  // IOC fills what it can, discards the rest
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 3);
        f.take();
        f.book.submit(2, Side::Buy, OrderType::IOC, 121, 8);
        check_events(f, {Event::trade(2, 1, Side::Buy, 120, 3)},
                     "IOC fills available quantity");
        check(f.book.order_count() == 0, name("IOC residual is discarded"));
    }
    {  // FOK: unfillable rejects before any fill; fillable fills exactly
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 3);
        f.book.submit(2, Side::Sell, OrderType::Limit, 121, 3);
        f.take();
        f.book.submit(3, Side::Buy, OrderType::FOK, 120, 5);
        check_events(
            f,
            {Event::rejected(3, Side::Buy, RejectReason::FokUnfillable, 120, 5)},
            "FOK beyond available quantity is rejected whole");
        check(f.book.quantity_at(Side::Sell, 120) == 3,
              name("rejected FOK leaves the book untouched"));
        f.book.submit(4, Side::Buy, OrderType::FOK, 121, 6);
        check_events(f,
                     {Event::trade(4, 1, Side::Buy, 120, 3),
                      Event::trade(4, 2, Side::Buy, 121, 3)},
                     "FOK within available quantity fills completely");
    }
    {  // cancel, then cancel again
        Fixture<Book> f;
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 5);
        f.take();
        f.book.cancel(1);
        check_events(f, {Event::cancelled(1, Side::Buy, 110, 5)},
                     "cancel emits Cancelled with remaining quantity");
        check(f.book.order_count() == 0 && !f.book.best_bid(),
              name("cancelled order leaves the book"));
        f.book.cancel(1);
        check_events(
            f, {Event::rejected(1, Side::Buy, RejectReason::UnknownOrder, 0, 0)},
            "cancelling a non-existent order is rejected");
    }
    {  // boundary rejections
        Fixture<Book> f;
        f.book.submit(0, Side::Buy, OrderType::Limit, 110, 5);
        check_events(
            f, {Event::rejected(0, Side::Buy, RejectReason::InvalidId, 110, 5)},
            "id zero is rejected");
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 0);
        check_events(
            f, {Event::rejected(1, Side::Buy, RejectReason::ZeroQuantity, 110, 0)},
            "zero quantity is rejected");
        f.book.submit(1, Side::Buy, OrderType::Limit, 99, 5);
        check_events(
            f,
            {Event::rejected(1, Side::Buy, RejectReason::PriceOutOfRange, 99, 5)},
            "price below the band is rejected");
        f.book.submit(1, Side::Buy, OrderType::Limit, 164, 5);
        check_events(
            f,
            {Event::rejected(1, Side::Buy, RejectReason::PriceOutOfRange, 164, 5)},
            "price above the band is rejected");
        f.book.submit(1, Side::Buy, OrderType::Limit, 163, 5);
        check_events(f, {Event::accepted(1, Side::Buy, 163, 5)},
                     "top band price is accepted");
    }
    {  // duplicate id: rejected while live, reusable after removal
        Fixture<Book> f;
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 5);
        f.take();
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 5);
        check_events(
            f, {Event::rejected(1, Side::Sell, RejectReason::DuplicateId, 120, 5)},
            "live duplicate id is rejected");
        f.book.cancel(1);
        f.take();
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 5);
        check_events(f, {Event::accepted(1, Side::Sell, 120, 5)},
                     "id is reusable after the previous order left the book");
    }
    {  // capacity: resting orders are bounded; matching still works when full
        Fixture<Book> f(2);
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 5);
        f.book.submit(2, Side::Buy, OrderType::Limit, 111, 5);
        f.take();
        f.book.submit(3, Side::Buy, OrderType::Limit, 112, 5);
        check_events(f,
                     {Event::rejected(3, Side::Buy,
                                      RejectReason::CapacityExhausted, 112, 5)},
                     "limit order into a full book is rejected");
        f.book.submit(4, Side::Sell, OrderType::IOC, 110, 7);
        check_events(f,
                     {Event::trade(4, 2, Side::Sell, 111, 5),
                      Event::trade(4, 1, Side::Sell, 110, 2)},
                     "IOC still matches against a full book");
    }
    {  // modify: reduction keeps queue position
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 10);
        f.book.submit(2, Side::Sell, OrderType::Limit, 120, 10);
        f.take();
        f.book.modify(1, 120, 4);
        check_events(f, {Event::reduced(1, Side::Sell, 120, 4)},
                     "quantity reduction emits Reduced");
        f.book.submit(3, Side::Buy, OrderType::Limit, 120, 4);
        check_events(f, {Event::trade(3, 1, Side::Buy, 120, 4)},
                     "reduced order keeps its queue position");
    }
    {  // modify: increase loses queue position
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 120, 5);
        f.book.submit(2, Side::Sell, OrderType::Limit, 120, 5);
        f.take();
        f.book.modify(1, 120, 8);
        check_events(f,
                     {Event::cancelled(1, Side::Sell, 120, 5),
                      Event::accepted(1, Side::Sell, 120, 8)},
                     "quantity increase is a cancel-replace");
        f.book.submit(3, Side::Buy, OrderType::Limit, 120, 5);
        check_events(f, {Event::trade(3, 2, Side::Buy, 120, 5)},
                     "increased order went to the back of the queue");
    }
    {  // modify: price change can cross immediately
        Fixture<Book> f;
        f.book.submit(1, Side::Sell, OrderType::Limit, 121, 5);
        f.book.submit(2, Side::Buy, OrderType::Limit, 119, 5);
        f.take();
        f.book.modify(2, 121, 5);
        check_events(f,
                     {Event::cancelled(2, Side::Buy, 119, 5),
                      Event::trade(2, 1, Side::Buy, 121, 5)},
                     "price-modify re-enters as an aggressive order");
    }
    {  // modify to zero cancels; modify unknown rejects
        Fixture<Book> f;
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 5);
        f.take();
        f.book.modify(1, 110, 0);
        check_events(f, {Event::cancelled(1, Side::Buy, 110, 5)},
                     "modify to zero quantity cancels");
        f.book.modify(9, 110, 5);
        check_events(
            f, {Event::rejected(9, Side::Buy, RejectReason::UnknownOrder, 110, 5)},
            "modify of an unknown id is rejected");
    }
    {  // snapshot shape
        Fixture<Book> f;
        f.book.submit(1, Side::Buy, OrderType::Limit, 110, 5);
        f.book.submit(2, Side::Buy, OrderType::Limit, 108, 3);
        f.book.submit(3, Side::Buy, OrderType::Limit, 110, 2);
        f.take();
        std::vector<ob::LevelView> rows(8);
        const std::size_t n = f.book.snapshot(Side::Buy, rows);
        check(n == 2 && rows[0] == ob::LevelView{110, 7, 2} &&
                  rows[1] == ob::LevelView{108, 3, 1},
              name("snapshot is best-first with aggregates"));
    }
    {  // instrument metadata carried, European by construction
        const ob::OptionSeries series{77, 20260925, 24000,
                                      ob::OptionRight::Call,
                                      ob::ExerciseStyle::European};
        Book book(series, 100, 64, 16);
        check(book.instrument() == series &&
                  book.instrument().style == ob::ExerciseStyle::European,
              name("book carries its European option series"));
    }
}

}  // namespace obtest
