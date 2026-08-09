#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "app/spsc_ring_buffer.hpp"
#include "gpu/gpu_model.hpp"
#include "gpu/gpu_worker.hpp"
#include "gpu/regression_oracle.hpp"
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
    const auto publish_result = [&result_ring](const std::uint64_t event_index,
                                               const std::int32_t bid,
                                               const std::int32_t ask) {
        verilator::RtlStreamResult* result = result_ring.try_reserve_push();
        REQUIRE(result != nullptr);
        result->event_index = event_index;
        result->features.valid = true;
        result->features.values[0] = 123;
        result->features.values[7] = market::fixed_point::kOne;
        result->best_bid_price_ticks = bid;
        result->best_ask_price_ticks = ask;
        result->best_bid_quantity = 1U;
        result->best_ask_quantity = 1U;
        result_ring.publish_push();
    };
    // Event one can buy at 101; one event later it can sell at 103, so it is a
    // genuine executable BUY training example rather than a midpoint proxy.
    publish_result(1U, 100, 101);
    publish_result(2U, 103, 104);

    gpu::ModelUpdateMailbox mailbox;
    gpu::GpuWorker worker(*model, result_ring, mailbox, 1U, 2U, 1U);
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
    REQUIRE(update->weights[7] > 0);
    REQUIRE(update->buy_threshold > update->sell_threshold);

    const gpu::GpuWorkerMetrics metrics = worker.metrics();
    REQUIRE(metrics.rtl_results_consumed == 2U);
    REQUIRE(metrics.invalid_feature_results_discarded == 0U);
    REQUIRE(metrics.valid_feature_rows_copied == 1U);
    REQUIRE(metrics.labelled_rows_created == 1U);
    REQUIRE(metrics.batches_submitted == 1U);
    REQUIRE(metrics.model_updates_published == 1U);
}

TEST_CASE("GPU regression batch matches the fixed-point CPU oracle") {
    using namespace market_engine;
    std::optional<gpu::GpuModel> model;
    try { model.emplace(); }
    catch (const app::OpenclSelectionError& error) { SKIP("No selectable OpenCL GPU on this machine: " + std::string(error.what())); }
    constexpr std::array<std::int32_t, 16> features{
        65536, 0, 0, 0, 0, 0, 0, 65536,
        32768, 0, 0, 0, 0, 0, 0, 65536,
    };
    constexpr std::array<std::int32_t, 2> labels{65536, -65536};
    const auto mapped = model->map_training_batch(labels.size());
    std::copy(features.begin(), features.end(), mapped.features.begin());
    std::copy(labels.begin(), labels.end(), mapped.labels.begin());
    gpu::ModelUpdate initial{};
    initial.update_version = 2U;
    initial.buy_threshold = 16384;
    initial.sell_threshold = -16384;
    const auto expected = gpu::run_regression_oracle(features, labels, initial, 66, 7);
    model->submit_training_batch(2U, 66, 7);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<gpu::TrainingUpdate> actual;
    while (!actual && std::chrono::steady_clock::now() < deadline) {
        actual = model->poll_training_update();
        if (!actual) std::this_thread::yield();
    }
    REQUIRE(actual.has_value());
    REQUIRE(actual->model.weights == expected.update.weights);
    REQUIRE(actual->model.buy_threshold == expected.update.buy_threshold);
    REQUIRE(actual->model.sell_threshold == expected.update.sell_threshold);
    REQUIRE(actual->metrics.squared_error_sum_q16 == expected.metrics.squared_error_sum_q16);
    REQUIRE(actual->metrics.correct_predictions == expected.metrics.correct_predictions);
    REQUIRE(actual->metrics.rows == expected.metrics.rows);
}
