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

// Build the initial RTL parameter bank directly from runtime configuration.
// The learner will later replace these values through a validated ModelUpdate.
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
    verilator::VerilatorRunner rtl_runner(config_.clock_period_ns);
    const market::ModelParameters initial_parameters = initial_model_parameters(config_);
    rtl_runner.write_model_parameters(initial_parameters);

    std::optional<gpu::GpuFeatureUploader> gpu_uploader;
    if (gpu_model != nullptr) gpu_uploader.emplace(*gpu_model);

    LiveResult result{};
    const std::size_t requested = event_limit ? static_cast<std::size_t>(*event_limit) : events.size();
    const std::size_t count = std::min(events.size(), requested);
    const auto started = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < count; ++index) {
        // Keep device transfers moving without making incoming event processing wait.
        if (gpu_uploader) gpu_uploader->poll_and_start();

        const verilator::RtlSnapshot snapshot = rtl_runner.process(events[index]);
        if (snapshot.error != market::BookError::None) {
            result.error = snapshot.error;
            result.failure_index = index;
            break;
        }
        if (gpu_uploader) {
            // The live GPU input always comes from the actual RTL feature output.
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
        gpu_uploader->drain();
        result.gpu_feature_batches_submitted = gpu_uploader->batches_submitted();
        result.gpu_feature_uploads_completed = gpu_uploader->uploads_completed();
    }

    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.final_rtl = rtl_runner.latest();
    result.active_parameters = initial_parameters;
    const verilator::RunnerMetrics metrics = rtl_runner.metrics();
    result.rtl_cycles = metrics.cycles;
    result.rtl_wall_seconds = metrics.wall_seconds;
    return result;
#else
    static_cast<void>(events);
    static_cast<void>(event_limit);
    static_cast<void>(gpu_model);
    throw std::runtime_error("--live requires a build with Verilator available");
#endif
}

}  // namespace market_engine::app
