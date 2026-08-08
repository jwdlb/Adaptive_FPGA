#include <catch2/catch_test_macros.hpp>

#include "gpu/gpu_model.hpp"

TEST_CASE("GPU smoke test proves repeated OpenCL setup or clearly skips without a GPU") {
    // Each call constructs and destroys a fresh GpuModel, exercising the RAII
    // cleanup of the context, queue, program, kernel, buffers, and events.
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto result = market_engine::gpu::run_gpu_smoke_test();
        if (result.status == market_engine::gpu::GpuSmokeTestStatus::skipped) {
            SKIP("No selectable OpenCL GPU on this machine: " + result.message);
        }

        REQUIRE(result.status == market_engine::gpu::GpuSmokeTestStatus::passed);
        REQUIRE(result.output == std::vector<float>{2.0F, 4.0F, 6.0F});
        REQUIRE(result.device.has_value());
    }
}
