#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "gpu/gpu_protocol.hpp"
#include "gpu/regression_oracle.hpp"
#include "market/fixed_point.hpp"

namespace {
using market_engine::gpu::FeatureSnapshot;
using market_engine::gpu::ModelUpdate;
using market_engine::market::fixed_point::from_double;

[[nodiscard]] ModelUpdate valid_update() {
    ModelUpdate update;
    update.update_version = 7U;
    update.weights = {from_double(0.1), from_double(-0.2), from_double(0.3), from_double(-0.4),
                      from_double(0.5), from_double(-0.6), from_double(0.7), from_double(0.8)};
    update.buy_threshold = from_double(0.25);
    update.sell_threshold = from_double(-0.25);
    return update;
}
}  // namespace

TEST_CASE("FeatureSnapshot keeps the RTL event metadata and eight features") {
    FeatureSnapshot snapshot{
        .event_index = 42U,
        .timestamp_ns = 123456U,
        .valid = true,
        .features = {1, -2, 3, -4, 5, -6, 7, 65536},
    };

    REQUIRE(snapshot.event_index == 42U);
    REQUIRE(snapshot.timestamp_ns == 123456U);
    REQUIRE(snapshot.valid);
    REQUIRE(snapshot.features.size() == 8U);
    REQUIRE(snapshot.features[7] == 65536);  // Constant 1.0 feature used for the bias.
}

TEST_CASE("ModelUpdate converts full weights bias and thresholds for RTL") {
    const ModelUpdate update = valid_update();
    const auto rtl_parameters = market_engine::gpu::model_parameters_from_update(update, 6U);

    REQUIRE(rtl_parameters.weights == update.weights);
    REQUIRE(rtl_parameters.weights[7] == from_double(0.8));  // The eighth coefficient is the bias.
    REQUIRE(rtl_parameters.buy_threshold == update.buy_threshold);
    REQUIRE(rtl_parameters.sell_threshold == update.sell_threshold);
    REQUIRE(rtl_parameters.model_version == update.update_version);
}

TEST_CASE("ModelUpdate rejects stale versions and invalid BUY SELL boundaries") {
    ModelUpdate update = valid_update();
    REQUIRE_NOTHROW(market_engine::gpu::validate_model_update(update, 6U));

    update.update_version = 6U;
    REQUIRE_THROWS_AS(market_engine::gpu::validate_model_update(update, 6U), std::invalid_argument);
    update = valid_update();
    update.buy_threshold = update.sell_threshold;
    REQUIRE_THROWS_AS(market_engine::gpu::validate_model_update(update, 6U), std::invalid_argument);
}

TEST_CASE("fixed-point regression oracle applies a mean batch gradient and L2 penalty") {
    ModelUpdate initial{};
    initial.update_version = 2U;
    initial.buy_threshold = from_double(0.25);
    initial.sell_threshold = from_double(-0.25);
    constexpr std::array<std::int32_t, 16> features{
        65536, 0, 0, 0, 0, 0, 0, 65536,
        65536, 0, 0, 0, 0, 0, 0, 65536,
    };
    constexpr std::array<std::int32_t, 2> labels{65536, 65536};
    const auto result = market_engine::gpu::run_regression_oracle(features, labels, initial, 65536, 0);
    REQUIRE(result.metrics.rows == 2U);
    REQUIRE(result.metrics.correct_predictions == 0U);
    REQUIRE(result.update.weights[0] == 65536);
    REQUIRE(result.update.weights[7] == 65536);
    const auto regularised = market_engine::gpu::run_regression_oracle(features, labels, result.update, 65536, 32768);
    REQUIRE(regularised.update.weights[0] < result.update.weights[0]);
}
