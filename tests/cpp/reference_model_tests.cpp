#include <catch2/catch_test_macros.hpp>

#include "app/config.hpp"
#include "reference/reference_model.hpp"

TEST_CASE("reference model processes a valid event", "[reference]") {
    market_engine::app::Config config{};
    market_engine::reference::ReferenceModel model(config);
    const market_engine::market::MarketEvent event{
        .timestamp_ns = 1U,
        .type = market_engine::market::EventType::Add,
        .side = market_engine::market::Side::Bid,
        .price_ticks = 100,
        .quantity = 10U,
    };

    const auto result = model.process(event, 0U);

    REQUIRE(result.error == market_engine::market::BookError::None);
    REQUIRE(model.book().bids[0].price_ticks == 100);
    REQUIRE(model.book().bids[0].quantity == 10U);
}
