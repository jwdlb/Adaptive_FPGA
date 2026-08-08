#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "gpu/feature_buffers.hpp"

namespace {
[[nodiscard]] market_engine::gpu::FeatureBatch batch(std::int32_t first_value) {
    market_engine::gpu::FeatureBatch result{};
    result[0][0] = first_value;
    return result;
}
}  // namespace

TEST_CASE("two input buffers alternate safely between replay filling and GPU work") {
    using market_engine::gpu::FeatureBufferPool;
    using market_engine::gpu::InputBufferState;

    FeatureBufferPool buffers;
    const std::size_t a = *buffers.acquire_for_filling();
    REQUIRE(a == 0U);
    buffers.finish_filling(a, batch(111));
    REQUIRE(buffers.state(a) == InputBufferState::Ready);

    const std::size_t gpu_a = *buffers.acquire_for_gpu();
    REQUIRE(gpu_a == a);
    REQUIRE(buffers.gpu_batch(gpu_a)[0][0] == 111);
    REQUIRE(buffers.state(gpu_a) == InputBufferState::InFlight);

    // While the GPU owns A, the replay side can fill B.
    const std::size_t b = *buffers.acquire_for_filling();
    REQUIRE(b == 1U);
    buffers.finish_filling(b, batch(222));
    REQUIRE(buffers.state(b) == InputBufferState::Ready);
    REQUIRE_FALSE(buffers.acquire_for_gpu().has_value());

    // Once A finishes, B becomes the next GPU input and A is reusable.
    buffers.finish_gpu_work(gpu_a);
    REQUIRE(buffers.state(a) == InputBufferState::Free);
    const std::size_t gpu_b = *buffers.acquire_for_gpu();
    REQUIRE(gpu_b == b);
    REQUIRE(buffers.gpu_batch(gpu_b)[0][0] == 222);
}

TEST_CASE("illegal input buffer ownership jumps are hard errors") {
    market_engine::gpu::FeatureBufferPool buffers;
    REQUIRE_THROWS_AS(buffers.finish_filling(0U, batch(1)), std::logic_error);

    buffers.begin_filling(0U);
    REQUIRE_THROWS_AS(buffers.begin_gpu_work(0U), std::logic_error);
    buffers.finish_filling(0U, batch(1));
    buffers.begin_gpu_work(0U);
    REQUIRE_THROWS_AS(buffers.begin_filling(0U), std::logic_error);  // InFlight -> Filling is forbidden.
    REQUIRE_THROWS_AS(buffers.finish_filling(1U, batch(2)), std::logic_error);
    buffers.cancel_gpu_work(0U);
    REQUIRE(buffers.state(0U) == market_engine::gpu::InputBufferState::Ready);
}
