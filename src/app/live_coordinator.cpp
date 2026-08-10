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
#include <condition_variable>
#include <exception>
#include <mutex>
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
                                gpu::GpuModel* const gpu_model,
                                const std::optional<market::ModelParameters> initial_model,
                                std::function<void(const market::ModelParameters&)> model_applied,
                                std::shared_ptr<DashboardSnapshotStore> dashboard_snapshots,
                                std::string input_name) const {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    const market::ModelParameters initial_parameters = initial_model.value_or(initial_model_parameters(config_));
    const std::size_t count = event_limit ?
        std::min(events.size(), static_cast<std::size_t>(*event_limit)) : events.size();
    const std::span<const market::MarketEvent> selected_events = events.first(count);

    // These two objects are the complete software transport around the
    // independently clocked RTL simulation. Only VerilatorWorker produces ring
    // values; only GpuWorker consumes them when GPU mode is enabled.
    app::SpscRingBuffer<verilator::RtlStreamResult> result_ring(kLiveResultRingCapacity);
    gpu::ModelUpdateMailbox update_mailbox;
    LiveResult result{};
    const auto started = std::chrono::steady_clock::now();

    struct ObservedState { std::mutex mutex; DashboardSnapshot snapshot; } observed;
    observed.snapshot.connection_state = dashboard_snapshots ? "connected" : "disabled";
    observed.snapshot.state = "running";
    observed.snapshot.activity_state = "running";
    observed.snapshot.activity_message = "Replaying market events through RTL";
    observed.snapshot.activity_total = count;
    observed.snapshot.input_file = std::move(input_name);
    observed.snapshot.total_events = count;
    observed.snapshot.queue_capacity = result_ring.capacity();
    observed.snapshot.model = initial_parameters;
    const auto unix_ms = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    std::mutex publisher_wait_mutex;
    std::condition_variable_any publisher_wake;
    std::jthread snapshot_publisher;
    if (dashboard_snapshots) {
        dashboard_snapshots->publish(observed.snapshot);
        snapshot_publisher = std::jthread([&, dashboard_snapshots](std::stop_token stop) {
            const auto interval = std::chrono::milliseconds(std::max(1U, 1000U / config_.dashboard_update_hz));
            while (!stop.stop_requested()) {
                std::unique_lock wait_lock(publisher_wait_mutex);
                if (publisher_wake.wait_for(wait_lock, stop, interval, [] { return false; })) break;
                wait_lock.unlock();
                DashboardSnapshot copy;
                { std::lock_guard lock(observed.mutex); copy = observed.snapshot; }
                copy.published_at_unix_ms = unix_ms();
                ++copy.sequence;
                { std::lock_guard lock(observed.mutex); observed.snapshot.sequence = copy.sequence; }
                dashboard_snapshots->publish(std::move(copy));
            }
        });
    }
    const auto observe_rtl = [&](const verilator::RtlSnapshot& rtl,
                                 const market::ModelParameters& model,
                                 const verilator::VerilatorWorkerMetrics& metrics) {
        std::lock_guard lock(observed.mutex);
        auto& snapshot = observed.snapshot;
        snapshot.processed_events = metrics.input_events_accepted;
        snapshot.activity_completed = metrics.input_events_accepted;
        snapshot.error_events = metrics.error_events;
        snapshot.book = rtl.book; snapshot.features = rtl.features; snapshot.signal = rtl.signal; snapshot.model = model;
        snapshot.queue_occupancy = result_ring.size();
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        snapshot.events_per_second = elapsed > 0.0 ? static_cast<double>(metrics.input_events_accepted) / elapsed : 0.0;
        snapshot.rtl_cycles_per_event = metrics.input_events_accepted == 0U ? 0.0 :
            static_cast<double>(metrics.rtl_cycles) / static_cast<double>(metrics.input_events_accepted);
        snapshot.recent_events.clear();
        const std::size_t end = std::min<std::size_t>(metrics.input_events_accepted, selected_events.size());
        const std::size_t begin = end > 12U ? end - 12U : 0U;
        snapshot.recent_events.insert(snapshot.recent_events.end(), selected_events.begin() + begin, selected_events.begin() + end);
    };
    auto applied_observer = [&, callback=std::move(model_applied)](const market::ModelParameters& model) {
        { std::lock_guard lock(observed.mutex); observed.snapshot.model = model; observed.snapshot.latest_update_unix_ms = unix_ms(); }
        if (callback) callback(model);
    };
    verilator::VerilatorWorker rtl_worker(selected_events, config_.clock_period_ns,
                                          initial_parameters, result_ring, update_mailbox,
                                          verilator::kDefaultResultBackpressureTimeout,
                                          std::move(applied_observer), observe_rtl);

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
                                  config_.feature_batch_size, initial_parameters.model_version + 1U,
                                  config_.label_horizon_events, 1,
                                  market::fixed_point::from_double(config_.learning_rate),
                                  market::fixed_point::from_double(config_.l2_regularisation),
                                  [&](const gpu::GpuWorkerMetrics& metrics) {
                                      std::lock_guard lock(observed.mutex);
                                      auto& snapshot = observed.snapshot;
                                      snapshot.gpu_batches = metrics.batches_submitted;
                                      snapshot.gpu_updates = metrics.model_updates_published;
                                      snapshot.squared_error_sum_q16 = metrics.latest_squared_error_sum_q16;
                                      snapshot.correct_predictions = metrics.latest_correct_predictions;
                                      snapshot.training_rows = metrics.latest_training_rows;
                                      snapshot.gpu_kernel_ms = metrics.latest_kernel_ms;
                                      snapshot.gpu_training_ms = metrics.latest_upload_ms + metrics.latest_kernel_ms + metrics.latest_readback_ms;
                                      snapshot.gpu_update_latency_ms = metrics.latest_update_latency_ms;
                                      snapshot.latest_update_unix_ms = unix_ms();
                                  });
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
        result.gpu_squared_error_sum_q16 = gpu_metrics.latest_squared_error_sum_q16;
        result.gpu_correct_predictions = gpu_metrics.latest_correct_predictions;
        result.gpu_training_rows = gpu_metrics.latest_training_rows;
        result.gpu_kernel_ms = gpu_metrics.latest_kernel_ms;
        result.gpu_upload_ms = gpu_metrics.latest_upload_ms;
        result.gpu_readback_ms = gpu_metrics.latest_readback_ms;
        result.gpu_update_latency_ms = gpu_metrics.latest_update_latency_ms;
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
    if (dashboard_snapshots) {
        snapshot_publisher.request_stop();
        publisher_wake.notify_all();
        snapshot_publisher.join();
        DashboardSnapshot final_snapshot;
        { std::lock_guard lock(observed.mutex); final_snapshot = observed.snapshot; }
        final_snapshot.state = result.error ? "failed" : "stopped";
        final_snapshot.activity_state = result.error ? "failed" : "completed";
        final_snapshot.activity_message = result.error ? "Replay stopped after a market-data error" : "Replay completed";
        final_snapshot.activity_completed = result.processed_events;
        final_snapshot.activity_total = count;
        final_snapshot.queue_occupancy = 0U;
        final_snapshot.processed_events = result.processed_events;
        final_snapshot.events_per_second = result.elapsed_seconds > 0.0 ? result.processed_events / result.elapsed_seconds : 0.0;
        final_snapshot.rtl_cycles_per_event = result.processed_events == 0U ? 0.0 : static_cast<double>(result.rtl_cycles) / result.processed_events;
        final_snapshot.published_at_unix_ms = unix_ms();
        ++final_snapshot.sequence;
        dashboard_snapshots->publish(std::move(final_snapshot));
    }
    return result;
#else
    // Keep the normal application honest in builds where Verilator was not
    // installed: live mode needs an RTL runner and cannot silently fall back.
    static_cast<void>(events);
    static_cast<void>(event_limit);
    static_cast<void>(gpu_model);
    static_cast<void>(dashboard_snapshots);
    static_cast<void>(input_name);
    throw std::runtime_error("--live requires a build with Verilator available");
#endif
}

}  // namespace market_engine::app
