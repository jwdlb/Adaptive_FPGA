#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "app/spsc_ring_buffer.hpp"
#include "gpu/gpu_model.hpp"
#include "gpu/gpu_worker.hpp"
#include "market/fixed_point.hpp"

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

TEST_CASE("GPU worker fills mapped streaming rows and publishes one complete model update") {
    using namespace market_engine;

    std::optional<gpu::GpuModel> model;
    try {
        model.emplace();
    } catch (const app::OpenclSelectionError& error) {
        SKIP("No selectable OpenCL GPU on this machine: " + std::string(error.what()));
    }

    app::SpscRingBuffer<verilator::RtlStreamResult> result_ring(3U);
    const auto publish_result = [&result_ring](const bool valid, const std::int32_t first_feature) {
        verilator::RtlStreamResult* result = result_ring.try_reserve_push();
        REQUIRE(result != nullptr);
        result->features.valid = valid;
        result->features.values[0] = first_feature;
        result_ring.publish_push();
    };
    // Invalid results are consumed but do not become GPU training rows.
    publish_result(false, 999);
    publish_result(true, 123);
    publish_result(true, 456);

    gpu::ModelUpdateMailbox mailbox;
    gpu::GpuWorker worker(*model, result_ring, mailbox, 2U, 2U);
    std::thread worker_thread([&] { worker.run(); });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!mailbox.has_update() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    worker.request_stop();
    worker_thread.join();

    const std::optional<gpu::ModelUpdate> update = mailbox.take();
    REQUIRE(update.has_value());
    REQUIRE(update->update_version == 2U);
    REQUIRE(update->weights[0] == 123);
    REQUIRE(update->weights[7] == market::fixed_point::kOne);
    REQUIRE(update->buy_threshold == 0);
    REQUIRE(update->sell_threshold == -market::fixed_point::kOne);

    const gpu::GpuWorkerMetrics metrics = worker.metrics();
    REQUIRE(metrics.rtl_results_consumed == 3U);
    REQUIRE(metrics.invalid_feature_results_discarded == 1U);
    REQUIRE(metrics.valid_feature_rows_copied == 2U);
    REQUIRE(metrics.batches_submitted == 1U);
    REQUIRE(metrics.model_updates_published == 1U);
}
