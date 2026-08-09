// This file implements the normal running path of the application:
//
//     market event source -> dedicated RTL worker -> optional GPU worker
//                                            ^                 |
//                                            |---- mailbox <----|
//
// It deliberately does not create a C++ reference answer or compare two
// implementations. Those checks live only in the test-support replay harness.
// Today the RTL pipeline is Verilator simulation; later the same coordinator
// boundary can be connected to a physical FPGA runner instead.
#include "app/live_coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <thread>

#include "gpu/gpu_model.hpp"
#include "gpu/gpu_worker.hpp"
#include "gpu/model_update_mailbox.hpp"
#include "market/fixed_point.hpp"
#include "verilator/verilator_worker.hpp"

namespace market_engine::app {
namespace {

// The host result ring is the normal multi-result backlog between the dedicated
// RTL worker and its one consumer. It is deliberately much larger than the RTL
// adapter's single held-result register, but still bounded so backpressure is
// explicit and safe if the GPU is temporarily slower.
inline constexpr std::size_t kLiveResultRingCapacity{1024U};

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
    const market::ModelParameters initial_parameters = initial_model_parameters(config_);
    const std::size_t count = event_limit ?
        std::min(events.size(), static_cast<std::size_t>(*event_limit)) : events.size();
    const std::span<const market::MarketEvent> selected_events = events.first(count);

    // These two objects are the complete software transport around the
    // independently clocked RTL simulation. Only VerilatorWorker produces ring
    // values; only GpuWorker consumes them when GPU mode is enabled.
    app::SpscRingBuffer<verilator::RtlStreamResult> result_ring(kLiveResultRingCapacity);
    gpu::ModelUpdateMailbox update_mailbox;
    verilator::VerilatorWorker rtl_worker(selected_events, config_.clock_period_ns,
                                          initial_parameters, result_ring, update_mailbox);

    LiveResult result{};
    const auto started = std::chrono::steady_clock::now();

    std::exception_ptr rtl_failure;
    std::atomic_bool rtl_failed{false};
    std::atomic_bool rtl_finished{false};
    std::thread rtl_thread([&] {
        try {
            rtl_worker.run();
        } catch (...) {
            rtl_failure = std::current_exception();
            rtl_failed.store(true, std::memory_order_release);
        }
        rtl_finished.store(true, std::memory_order_release);
    });

    if (gpu_model != nullptr) {
        // This is the normal adaptive route. The GPU thread copies a result out
        // of the ring immediately, so it never holds a ring slot while OpenCL
        // runs. Its complete replacement model returns through update_mailbox.
        gpu::GpuWorker gpu_worker(*gpu_model, result_ring, update_mailbox,
                                  config_.feature_batch_size, 2U,
                                  config_.label_horizon_events, 1,
                                  market::fixed_point::from_double(config_.learning_rate),
                                  market::fixed_point::from_double(config_.l2_regularisation));
        std::exception_ptr gpu_failure;
        std::atomic_bool gpu_failed{false};
        std::thread gpu_thread([&] {
            try {
                gpu_worker.run();
            } catch (...) {
                gpu_failure = std::current_exception();
                gpu_failed.store(true, std::memory_order_release);
                rtl_worker.request_stop();
            }
        });

        // Wait until the RTL worker has published the final output result, but
        // keep it alive afterwards so it can apply the GPU's final ModelUpdate.
        while (!rtl_worker.stream_drained() && !gpu_failed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (gpu_failed.load(std::memory_order_acquire) || rtl_failed.load(std::memory_order_acquire)) {
            gpu_worker.request_stop();
        } else {
            // This does not stop the GPU immediately: it means "no more RTL
            // results will arrive; drain the ring, finish the last update, then
            // return". That final update is what the still-running RTL worker
            // will apply below.
            gpu_worker.request_input_complete();
        }
        gpu_thread.join();

        if (gpu_failure) std::rethrow_exception(gpu_failure);

        // GPU publication is now complete. Closing is the precise signal that
        // lets the RTL worker apply a final waiting update and leave its idle
        // loop. No later GPU thread can write this mailbox.
        update_mailbox.close();
        rtl_thread.join();
        if (rtl_failure) std::rethrow_exception(rtl_failure);

        const gpu::GpuWorkerMetrics gpu_metrics = gpu_worker.metrics();
        result.gpu_rtl_results_consumed = gpu_metrics.rtl_results_consumed;
        result.gpu_valid_feature_rows_copied = gpu_metrics.valid_feature_rows_copied;
        result.gpu_batches_submitted = gpu_metrics.batches_submitted;
        result.gpu_model_updates_published = gpu_metrics.model_updates_published;
    } else {
        // A non-GPU live run still needs one ring consumer: it releases compact
        // RTL results as soon as they arrive, without building CPU batches or
        // performing any reference-model work.
        // With no GPU producer, there can be no later mailbox update, so close
        // it before the RTL worker reaches its post-stream idle stage.
        update_mailbox.close();
        while (!rtl_finished.load(std::memory_order_acquire) || !result_ring.empty()) {
            if (const verilator::RtlStreamResult* stream_result = result_ring.try_begin_pop();
                stream_result != nullptr) {
                if (!result.error && stream_result->error != market::BookError::None) {
                    result.error = stream_result->error;
                    result.failure_index = static_cast<std::size_t>(stream_result->event_index);
                }
                result_ring.finish_pop();
            } else {
                std::this_thread::yield();
            }
        }
        rtl_thread.join();
        if (rtl_failure) std::rethrow_exception(rtl_failure);
    }

    result.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    result.final_rtl = rtl_worker.latest();
    result.active_parameters = rtl_worker.active_parameters();
    const verilator::VerilatorWorkerMetrics rtl_metrics = rtl_worker.metrics();
    result.processed_events = rtl_metrics.input_events_accepted;
    result.rtl_stream_results_published = rtl_metrics.stream_results_published;
    result.rtl_model_updates_applied = rtl_metrics.model_updates_applied;
    result.rtl_cycles = rtl_metrics.rtl_cycles;
    result.rtl_wall_seconds = result.elapsed_seconds;
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
