// Minimal end-to-end demonstration: one European option series, one book,
// a short session of orders, the resulting event stream and a level-2
// snapshot. This is the "being able to see the book is worth an hour"
// artifact from the build manual, kept current so the README example never
// drifts from the real interface.
//
//     cmake --build build --target ob_demo && ./build/ob_demo

#include <cstdio>
#include <vector>

#include "ob/book_fast.hpp"
#include "ob/events.hpp"
#include "ob/instrument.hpp"

namespace {

const char* name_of(ob::EventType t) {
    switch (t) {
        case ob::EventType::Accepted: return "Accepted ";
        case ob::EventType::Trade: return "Trade    ";
        case ob::EventType::Reduced: return "Reduced  ";
        case ob::EventType::Cancelled: return "Cancelled";
        case ob::EventType::Rejected: return "Rejected ";
    }
    return "?";
}

// A sink is any callable taking `const Event&` — this one narrates.
struct PrintSink {
    void operator()(const ob::Event& e) const {
        std::printf("  %s id=%llu", name_of(e.type),
                    static_cast<unsigned long long>(e.id));
        if (e.type == ob::EventType::Trade) {
            std::printf(" maker=%llu",
                        static_cast<unsigned long long>(e.maker_id));
        }
        std::printf(" %s px=%lld qty=%llu",
                    e.side == ob::Side::Buy ? "Buy " : "Sell",
                    static_cast<long long>(e.price),
                    static_cast<unsigned long long>(e.qty));
        if (e.type == ob::EventType::Rejected) {
            std::printf(" reason=%d", static_cast<int>(e.reason));
        }
        std::printf("\n");
    }
};

template <typename Book>
void print_depth(const Book& book) {
    std::vector<ob::LevelView> rows(8);
    std::printf("  %-28s | %s\n", "BIDS (best first)", "ASKS (best first)");
    const std::size_t nb = book.snapshot(ob::Side::Buy, rows);
    std::vector<ob::LevelView> bids(rows.begin(),
                                    rows.begin() + static_cast<long>(nb));
    const std::size_t na = book.snapshot(ob::Side::Sell, rows);
    const std::size_t n = nb > na ? nb : na;
    for (std::size_t i = 0; i < n; ++i) {
        char left[64] = "";
        char right[64] = "";
        if (i < nb) {
            std::snprintf(left, sizeof left, "%llu @ %lld (%u orders)",
                          static_cast<unsigned long long>(bids[i].qty),
                          static_cast<long long>(bids[i].price),
                          bids[i].orders);
        }
        if (i < na) {
            std::snprintf(right, sizeof right, "%llu @ %lld (%u orders)",
                          static_cast<unsigned long long>(rows[i].qty),
                          static_cast<long long>(rows[i].price),
                          rows[i].orders);
        }
        std::printf("  %-28s | %s\n", left, right);
    }
}

}  // namespace

int main() {
    // NIFTY 24000 CE, expiring 25 Sep 2026 — European, like all index
    // options; the style is the only one the type system can express.
    const ob::OptionSeries series{
        1, 20260925, 24000, ob::OptionRight::Call, ob::ExerciseStyle::European};

    // Premium quoted in ticks of 0.05 rupees: band [0, 16384) ticks,
    // capacity 4096 resting orders. All pre-allocated here; no further
    // allocation for the life of the book.
    ob::FastBook<PrintSink> book(series, 0, 16384, 4096);

    std::printf("series: underlying=%u expiry=%u strike=%lld right=%s "
                "style=European\n\n",
                book.instrument().underlying_id, book.instrument().expiry,
                static_cast<long long>(book.instrument().strike),
                book.instrument().right == ob::OptionRight::Call ? "Call"
                                                                 : "Put");

    std::printf("build the book:\n");
    book.submit(1, ob::Side::Buy, ob::OrderType::Limit, 2495, 40);
    book.submit(2, ob::Side::Buy, ob::OrderType::Limit, 2490, 75);
    book.submit(3, ob::Side::Buy, ob::OrderType::Limit, 2495, 25);
    book.submit(4, ob::Side::Sell, ob::OrderType::Limit, 2505, 50);
    book.submit(5, ob::Side::Sell, ob::OrderType::Limit, 2510, 90);
    std::printf("\ndepth:\n");
    print_depth(book);

    std::printf("\nan aggressive buy for 60 @ 2510 sweeps the ask:\n");
    book.submit(6, ob::Side::Buy, ob::OrderType::Limit, 2510, 60);
    std::printf("\nreduce order 2 to 30 (keeps queue position), cancel 3:\n");
    book.modify(2, 2490, 30);
    book.cancel(3);
    std::printf("\ndepth after:\n");
    print_depth(book);

    std::printf("\nbest bid=%lld best ask=%lld, %zu orders resting\n",
                static_cast<long long>(*book.best_bid()),
                static_cast<long long>(*book.best_ask()),
                book.order_count());
    return 0;
}
