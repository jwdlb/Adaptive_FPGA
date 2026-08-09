// This file implements the normal running path of the application:
//
//     market event source -> RTL pipeline -> optional GPU feature upload
//
// It deliberately does not create a C++ reference answer or compare two
// implementations. Those checks live only in the test-support replay harness.
// Today the RTL pipeline is Verilator simulation; later the same coordinator
// boundary can be connected to a physical FPGA runner instead.
#include "app/live_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>

#include "gpu/feature_uploader.hpp"
#include "gpu/gpu_model.hpp"
#include "market/fixed_point.hpp"

namespace market_engine::app {
namespace {

// Build the initial model which the RTL activates before it receives its first
// event. There are eight zero-initialised weights (including the bias weight),
// then the configured BUY and SELL score boundaries, labelled as model version
// one. The future GPU learner replaces this whole model atomically.
[[nodiscard]] market::ModelParameters initial_model_parameters(const Config& config) noexcept {
    market::ModelParameters parameters{};
    parameters.buy_threshold = market::fixed_point::from_double(config.buy_threshold);
    parameters.sell_threshold = market::fixed_point::from_double(config.sell_threshold);
    parameters.model_version = 1U;
    return parameters;
}

}  // namespace

LiveResult LiveCoordinator::run(const std::span<const market::MarketEvent> events,
                                const std::optional<std::uint64_t> event_limit,
                                gpu::GpuModel* const gpu_model) const {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    // Create and reset the operational RTL runner. This is the component that
    // clocks the simulated hardware and returns its order book, features, and
    // trading signal after each submitted event.
    verilator::VerilatorRunner rtl_runner(config_.clock_period_ns);
    const market::ModelParameters initial_parameters = initial_model_parameters(config_);
    // Write all weights and thresholds into the RTL before processing data, so
    // every event uses one known, complete starting model.
    rtl_runner.write_model_parameters(initial_parameters);

    // GPU operation is optional: without a GpuModel, live mode remains a pure
    // event -> RTL application. With one, this uploader batches valid RTL
    // features and schedules their non-blocking transfer to the GPU.
    std::optional<gpu::GpuFeatureUploader> gpu_uploader;
    if (gpu_model != nullptr) gpu_uploader.emplace(*gpu_model);

    LiveResult result{};
    // `--events N` limits the input. Otherwise process the complete supplied
    // event stream. std::span means the coordinator reads these events but does
    // not own, copy, or alter the caller's event list.
    const std::size_t requested = event_limit ? static_cast<std::size_t>(*event_limit) : events.size();
    const std::size_t count = std::min(events.size(), requested);
    const auto started = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < count; ++index) {
        // Check whether a previous GPU upload has completed and start a waiting
        // batch if possible. This only polls; it never pauses this event loop.
        if (gpu_uploader) gpu_uploader->poll_and_start();

        // Drive exactly one market event through the RTL and wait for that RTL
        // operation to finish. The snapshot is the real output of this path,
        // including the updated book, calculated features, and BUY/SELL/HOLD.
        const verilator::RtlSnapshot snapshot = rtl_runner.process(events[index]);
        if (snapshot.error != market::BookError::None) {
            // A rejected event means the operational order book did not accept
            // it. Stop here rather than silently continuing from bad input.
            result.error = snapshot.error;
            result.failure_index = index;
            break;
        }
        if (gpu_uploader) {
            // The GPU receives only the feature values that RTL actually
            // produced. Invalid feature snapshots are passed on too, but the
            // uploader's collector ignores them until it has 32 valid ones.
            gpu_uploader->add_snapshot({
                .event_index = snapshot.signal.event_index,
                .timestamp_ns = snapshot.signal.timestamp_ns,
                .valid = snapshot.features.valid,
                .features = snapshot.features.values,
            });
        }
        ++result.processed_events;
    }

    if (gpu_uploader) {
        // At end of stream it is safe to wait for the final A/B upload. This
        // protects the CPU-side source batch until OpenCL is done reading it.
        gpu_uploader->drain();
        result.gpu_feature_batches_submitted = gpu_uploader->batches_submitted();
        result.gpu_feature_uploads_completed = gpu_uploader->uploads_completed();
    }

    // Package the final state and measurements for main.cpp to print. At this
    // stage active_parameters is still the initial model; later it will track
    // validated GPU ModelUpdates applied to the RTL.
    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.final_rtl = rtl_runner.latest();
    result.active_parameters = initial_parameters;
    const verilator::RunnerMetrics metrics = rtl_runner.metrics();
    result.rtl_cycles = metrics.cycles;
    result.rtl_wall_seconds = metrics.wall_seconds;
    return result;
#else
    // Keep the normal application honest in builds where Verilator was not
    // installed: live mode needs an RTL runner and cannot silently fall back.
    static_cast<void>(events);
    static_cast<void>(event_limit);
    static_cast<void>(gpu_model);
    throw std::runtime_error("--live requires a build with Verilator available");
#endif
}

}  // namespace market_engine::app
