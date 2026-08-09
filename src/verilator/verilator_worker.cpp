#include "verilator/verilator_worker.hpp"

#include <optional>
#include <stdexcept>
#include <thread>

#include "gpu/gpu_protocol.hpp"

namespace market_engine::verilator {

VerilatorWorker::VerilatorWorker(
    const std::span<const market::MarketEvent> events,
    const std::uint32_t clock_period_ns,
    market::ModelParameters initial_parameters,
    app::SpscRingBuffer<RtlStreamResult>& result_ring,
    gpu::ModelUpdateMailbox& update_mailbox,
    const std::chrono::steady_clock::duration backpressure_timeout,
    std::function<void(const market::ModelParameters&)> model_applied)
    : events_(events), result_ring_(result_ring), update_mailbox_(update_mailbox),
      runner_(clock_period_ns), active_parameters_(std::move(initial_parameters)),
      backpressure_timeout_(backpressure_timeout), model_applied_(std::move(model_applied)) {
    if (backpressure_timeout_ <= std::chrono::steady_clock::duration::zero()) {
        throw std::invalid_argument("RTL result backpressure timeout must be positive");
    }

    // The worker owns the only runner, so it also establishes the model the RTL
    // will use before the first queued CSV event is offered.
    runner_.write_model_parameters(active_parameters_);
    active_parameters_.update_count = runner_.latest().update_count;
}

void VerilatorWorker::apply_waiting_model_update() {
    const std::optional<gpu::ModelUpdate> update = update_mailbox_.take();
    if (!update) return;

    // Convert and validate the complete GPU replacement before any value reaches
    // the shadow bank. The runner writes all ten parameter words then performs the
    // existing atomic RTL commit while the worker has not offered an event.
    active_parameters_ = gpu::model_parameters_from_update(*update, active_parameters_.model_version);
    runner_.write_model_parameters(active_parameters_);
    active_parameters_.update_count = runner_.latest().update_count;
    ++metrics_.model_updates_applied;
    if (model_applied_) model_applied_(active_parameters_);
}

void VerilatorWorker::run() {
    std::size_t next_event{};
    std::optional<std::chrono::steady_clock::time_point> backpressure_started;

    // First drain the input stream into the result ring. After that, remain alive
    // until the GPU worker closes its mailbox: the final full GPU batch may only
    // finish after the final RTL result was published, and its replacement model
    // still deserves one atomic RTL apply before this worker exits.
    while (!stop_requested_.load(std::memory_order_relaxed)) {
        const bool stream_has_work =
            next_event < events_.size() ||
            metrics_.stream_results_published < metrics_.input_events_accepted ||
            runner_.stream_result_valid();
        if (!stream_has_work) {
            stream_drained_.store(true, std::memory_order_release);
            apply_waiting_model_update();
            if (update_mailbox_.closed() && !update_mailbox_.has_update()) break;
            // Keep advancing the simulation while waiting for the separate GPU
            // thread, so a model-bank commit can complete normally.
            static_cast<void>(runner_.step(nullptr, false));
            std::this_thread::yield();
            continue;
        }
        RtlStreamResult* reserved_result_slot = nullptr;
        bool accept_stream_result = false;

        if (runner_.stream_result_valid()) {
            reserved_result_slot = result_ring_.try_reserve_push();
            if (reserved_result_slot != nullptr) {
                // The adapter promises these fields stay stable while valid is
                // high and ready is low. Decode straight into SPSC storage before
                // the handshake clock releases the RTL register.
                runner_.read_stream_result_into(*reserved_result_slot);
                accept_stream_result = true;

                if (backpressure_started) {
                    metrics_.result_backpressure_time +=
                        std::chrono::steady_clock::now() - *backpressure_started;
                    backpressure_started.reset();
                }
            } else if (!backpressure_started) {
                backpressure_started = std::chrono::steady_clock::now();
            }
        } else {
            backpressure_started.reset();
        }

        if (backpressure_started &&
            std::chrono::steady_clock::now() - *backpressure_started > backpressure_timeout_) {
            throw std::runtime_error("RTL result ring stayed full beyond the backpressure timeout");
        }

        // Model updates are applied only while no output is waiting and the core is
        // idle/ready for a new input. This makes the existing RTL shadow-bank
        // commit an event-boundary operation.
        if (!runner_.stream_result_valid() && runner_.input_ready()) {
            apply_waiting_model_update();
        }

        const market::MarketEvent* const input_event =
            next_event < events_.size() ? &events_[next_event] : nullptr;
        const RunnerStep step = runner_.step(input_event, accept_stream_result);

        if (reserved_result_slot != nullptr) {
            if (!step.result_accepted) {
                // This should be impossible because the slot was reserved only
                // after result_valid was observed. Do not leak that reservation if
                // a future runner implementation violates the contract.
                result_ring_.cancel_push();
                throw std::logic_error("RTL stream result disappeared before its ready handshake");
            }
            result_ring_.publish_push();
            ++metrics_.stream_results_published;
        }

        if (step.input_accepted) {
            ++next_event;
            ++metrics_.input_events_accepted;
        }

        // A full SPSC ring deliberately keeps result_ready low. Yielding here gives
        // the separate GPU consumer thread an immediate chance to release a slot;
        // the worker still advances the RTL clock on its next loop iteration.
        if (runner_.stream_result_valid() && reserved_result_slot == nullptr) {
            std::this_thread::yield();
        }
    }

    if (backpressure_started) {
        metrics_.result_backpressure_time += std::chrono::steady_clock::now() - *backpressure_started;
    }
    stream_drained_.store(true, std::memory_order_release);
    metrics_.rtl_cycles = runner_.metrics().cycles;
}

void VerilatorWorker::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

bool VerilatorWorker::stream_drained() const noexcept {
    return stream_drained_.load(std::memory_order_acquire);
}

VerilatorWorkerMetrics VerilatorWorker::metrics() const noexcept { return metrics_; }

const market::ModelParameters& VerilatorWorker::active_parameters() const noexcept {
    return active_parameters_;
}

const RtlSnapshot& VerilatorWorker::latest() const noexcept { return runner_.latest(); }

}  // namespace market_engine::verilator
