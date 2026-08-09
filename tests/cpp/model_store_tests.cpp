#include <chrono>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "app/model_store.hpp"

namespace {
std::filesystem::path temporary_model_path() {
    return std::filesystem::temp_directory_path() /
        ("adaptive-fpga-model-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
}

market_engine::market::ModelParameters valid_model() {
    market_engine::market::ModelParameters model{};
    model.weights = {1, -2, 3, -4, 5, -6, 7, 8};
    model.buy_threshold = 16384; model.sell_threshold = -16384;
    model.model_version = 9U; model.update_count = 4U;
    return model;
}
}  // namespace

TEST_CASE("model files round trip exact canonical Q16 values") {
    const auto path = temporary_model_path();
    const auto expected = valid_model();
    market_engine::app::save_model_file_atomically(path, expected);
    const auto actual = market_engine::app::load_model_file(path);
    std::filesystem::remove(path);
    REQUIRE(actual.weights == expected.weights);
    REQUIRE(actual.buy_threshold == expected.buy_threshold);
    REQUIRE(actual.sell_threshold == expected.sell_threshold);
    REQUIRE(actual.model_version == expected.model_version);
    REQUIRE(actual.update_count == expected.update_count);
}

TEST_CASE("model files reject malformed incompatible and unsafe state") {
    const auto path = temporary_model_path();
    { std::ofstream output(path); output << "{\"schemaVersion\":2}"; }
    REQUIRE_THROWS_AS(market_engine::app::load_model_file(path), market_engine::app::ModelStoreError);
    { std::ofstream output(path, std::ios::trunc); output << "{\"schemaVersion\":1,\"modelVersion\":0,\"updateCount\":0,\"weightsQ16\":[0,0,0,0,0,0,0,0],\"buyThresholdQ16\":0,\"sellThresholdQ16\":0}"; }
    REQUIRE_THROWS_AS(market_engine::app::load_model_file(path), market_engine::app::ModelStoreError);
    std::filesystem::remove(path);
}
