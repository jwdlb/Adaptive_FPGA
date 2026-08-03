#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "market/fixed_point.hpp"
#include "market/order_book.hpp"

namespace {
using namespace market_engine::market;

[[nodiscard]] MarketEvent event(EventType type, Side side, std::int32_t price, std::uint32_t quantity) {
    return {1000U, type, side, price, quantity};
}
}  // namespace

TEST_CASE("order book inserts, aggregates, orders, and bounds levels") {
    OrderBook book;
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 100, 10U)).error == BookError::None);
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 102, 20U)).error == BookError::None);
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 101, 30U)).error == BookError::None);
    REQUIRE(book.snapshot().bids[0] == PriceLevel{102, 20U});
    REQUIRE(book.snapshot().bids[1] == PriceLevel{101, 30U});
    REQUIRE(book.snapshot().bids[2] == PriceLevel{100, 10U});
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 101, std::numeric_limits<std::uint32_t>::max())).error == BookError::None);
    REQUIRE(book.snapshot().bids[1].quantity == std::numeric_limits<std::uint32_t>::max());
    for (std::int32_t price = 99; price >= 90; --price) REQUIRE(book.apply(event(EventType::Add, Side::Bid, price, 1U)).error == BookError::None);
    REQUIRE(book.snapshot().bids[9].price_ticks == 93);
    REQUIRE(book.check_invariants().empty());
}

TEST_CASE("order book updates, cancels, trades, and preserves state on errors") {
    OrderBook book;
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 100, 10U)).error == BookError::None);
    REQUIRE(book.apply(event(EventType::Add, Side::Ask, 102, 20U)).error == BookError::None);
    REQUIRE(book.apply(event(EventType::Update, Side::Bid, 100, 7U)).error == BookError::None);
    REQUIRE(book.snapshot().bids[0].quantity == 7U);
    const auto cancel = book.apply(event(EventType::Cancel, Side::Bid, 100, 3U));
    REQUIRE(cancel.order_flow_delta == -3);
    const auto trade = book.apply(event(EventType::Trade, Side::Ask, 102, 5U));
    REQUIRE(trade.trade_flow_delta == 5);
    REQUIRE(book.snapshot().asks[0].quantity == 15U);
    const auto before = book.snapshot();
    REQUIRE(book.apply(event(EventType::Update, Side::Bid, 999, 1U)).error == BookError::MissingPrice);
    REQUIRE(book.snapshot() == before);
    REQUIRE(book.apply(event(EventType::Update, Side::Bid, 100, 0U)).error == BookError::None);
    REQUIRE(book.snapshot().bids[0].quantity == 0U);
}

TEST_CASE("crossed events are rejected without changing the book") {
    OrderBook book;
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 100, 1U)).error == BookError::None);
    const auto before = book.snapshot();
    REQUIRE(book.apply(event(EventType::Add, Side::Ask, 100, 1U)).error == BookError::CrossedBook);
    REQUIRE(book.snapshot() == before);
}

TEST_CASE("features, strategy, rolling window, and checksum are deterministic") {
    OrderBook book;
    REQUIRE(book.apply(event(EventType::Add, Side::Bid, 100, 20U)).error == BookError::None);
    REQUIRE(book.apply(event(EventType::Add, Side::Ask, 102, 10U)).error == BookError::None);
    FeatureWindow window;
    window.push(10, 0, 1);
    const auto features = calculate_features(book, window);
    REQUIRE(features.valid);
    REQUIRE(features.values[1] > 0);
    REQUIRE(features.values[7] == fixed_point::kOne);
    ModelParameters parameters{};
    parameters.weights[7] = fixed_point::kOne;
    parameters.buy_threshold = fixed_point::from_double(0.5);
    parameters.sell_threshold = fixed_point::from_double(-0.5);
    parameters.model_version = 3U;
    const auto signal = evaluate_strategy(1000U, 1U, features, parameters);
    REQUIRE(signal.valid);
    REQUIRE(signal.action == Action::Buy);
    REQUIRE(deterministic_checksum(book.snapshot(), features, signal, parameters) ==
            deterministic_checksum(book.snapshot(), features, signal, parameters));
}

TEST_CASE("feature window expires exactly the oldest event after 64 entries") {
    FeatureWindow window;
    for (std::int64_t value = 1; value <= 64; ++value) window.push(value, -value, value);
    REQUIRE(window.size() == 64U);
    REQUIRE(window.order_flow_sum() == 2080);
    REQUIRE(window.trade_flow_sum() == -2080);
    window.push(65, -65, 65);
    REQUIRE(window.size() == 64U);
    REQUIRE(window.order_flow_sum() == 2144);  // 2 + ... + 65
    REQUIRE(window.trade_flow_sum() == -2144);
    REQUIRE(window.absolute_midpoint_change_sum() == 2144);
}
