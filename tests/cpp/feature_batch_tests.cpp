#include <cstddef>

#include <catch2/catch_test_macros.hpp>

#include "gpu/gpu_protocol.hpp"

namespace {
[[nodiscard]] market_engine::gpu::FeatureSnapshot snapshot(std::int32_t base, bool valid = true) {
    market_engine::gpu::FeatureSnapshot value;
    value.valid = valid;
    for (std::size_t feature = 0; feature < value.features.size(); ++feature) {
        value.features[feature] = base + static_cast<std::int32_t>(feature);
    }
    return value;
}
}  // namespace

TEST_CASE("FeatureBatch is contiguous 32 by 8 Q16.16 values in time feature order") {
    REQUIRE(sizeof(market_engine::gpu::FeatureBatch) == 32U * 8U * sizeof(std::int32_t));

    market_engine::gpu::FeatureBatchCollector collector;
    REQUIRE_FALSE(collector.add(snapshot(999, false)).has_value());
    REQUIRE(collector.size() == 0U);  // Invalid RTL snapshots never enter the GPU input.

    for (std::int32_t time = 0; time < 31; ++time) {
        REQUIRE_FALSE(collector.add(snapshot(time * 10)).has_value());
    }
    REQUIRE(collector.size() == 31U);

    const auto completed = collector.add(snapshot(310));
    REQUIRE(completed.has_value());
    REQUIRE(collector.size() == 0U);
    REQUIRE((*completed)[0][0] == 0);
    REQUIRE((*completed)[0][7] == 7);
    REQUIRE((*completed)[31][0] == 310);
    REQUIRE((*completed)[31][7] == 317);

    // A following valid snapshot begins the next non-overlapping 32-step input.
    REQUIRE_FALSE(collector.add(snapshot(1000)).has_value());
    REQUIRE(collector.size() == 1U);
}
