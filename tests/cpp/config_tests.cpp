#include <sstream>

#include <catch2/catch_test_macros.hpp>

#include "app/config.hpp"

using market_engine::app::Config;
using market_engine::app::ConfigError;

TEST_CASE("default configuration parses and validates") {
    std::istringstream input(R"({"orderBookDepth":10,"featureBatchSize":1024,"buyThreshold":0.2,"sellThreshold":-0.2})");
    const Config config = market_engine::app::parse_config(input);
    REQUIRE(config.order_book_depth == 10U);
    REQUIRE(config.feature_batch_size == 1024U);
    REQUIRE(config.learning_rate == 0.001);
}

TEST_CASE("version one rejects an unsupported book depth") {
    Config config;
    config.order_book_depth = 9U;
    REQUIRE_THROWS_AS(market_engine::app::validate_config(config), ConfigError);
}

TEST_CASE("configuration rejects invalid learning and threshold values") {
    Config config;
    config.learning_rate = 0.0;
    REQUIRE_THROWS_AS(market_engine::app::validate_config(config), ConfigError);

    config = Config{};
    config.buy_threshold = config.sell_threshold;
    REQUIRE_THROWS_AS(market_engine::app::validate_config(config), ConfigError);
}

TEST_CASE("configuration rejects malformed JSON") {
    std::istringstream input("{");
    REQUIRE_THROWS_AS(market_engine::app::parse_config(input), ConfigError);
}

