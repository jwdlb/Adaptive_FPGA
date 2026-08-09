// This file declares the dedicated RTL execution worker used by the normal
// continuous path. It is the only component that owns VerilatorRunner: it feeds
// preloaded market events, advances the simulated clock, writes results directly
// into SPSC slots, and applies complete GPU ModelUpdates at safe boundaries.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

#include "app/spsc_ring_buffer.hpp"
#include "gpu/model_update_mailbox.hpp"
#include "market/event.hpp"
#include "verilator/rtl_stream.hpp"
#include "verilator/verilator_runner.hpp"

namespace market_engine::verilator {

inline constexpr std::chrono::seconds kDefaultResultBackpressureTimeout{10};

struct VerilatorWorkerMetrics {
    std::size_t input_events_accepted{};
    std::size_t stream_results_published{};
    std::size_t model_updates_applied{};
    std::uint64_t rtl_cycles{};
    std::chrono::nanoseconds result_backpressure_time{};
};

class VerilatorWorker {
public:
    VerilatorWorker(std::span<const market::MarketEvent> events,
                    std::uint32_t clock_period_ns,
                    market::ModelParameters initial_parameters,
                    app::SpscRingBuffer<RtlStreamResult>& result_ring,
                    gpu::ModelUpdateMailbox& update_mailbox,
                    std::chrono::steady_clock::duration backpressure_timeout =
                        kDefaultResultBackpressureTimeout,
                    std::function<void(const market::ModelParameters&)> model_applied = {});

    VerilatorWorker(const VerilatorWorker&) = delete;
    VerilatorWorker& operator=(const VerilatorWorker&) = delete;

    // Run the worker loop on its dedicated caller-owned thread. It returns after
    // all offered events and their held results are drained, or throws if the
    // result ring remains full for the configured timeout.
    void run();

    // Request an orderly early stop. The worker observes this between simulated
    // clocks; it never allows an already held RTL result to be overwritten.
    void request_stop() noexcept;

    // True once every offered event has been accepted and every corresponding
    // result has entered the SPSC ring. The worker can then remain alive only to
    // apply a final GPU ModelUpdate before the mailbox is closed.
    [[nodiscard]] bool stream_drained() const noexcept;

    // Read these only after run() has returned or the worker thread has joined.
    [[nodiscard]] VerilatorWorkerMetrics metrics() const noexcept;
    [[nodiscard]] const market::ModelParameters& active_parameters() const noexcept;
    [[nodiscard]] const RtlSnapshot& latest() const noexcept;

private:
    void apply_waiting_model_update();

    std::span<const market::MarketEvent> events_;
    app::SpscRingBuffer<RtlStreamResult>& result_ring_;
    gpu::ModelUpdateMailbox& update_mailbox_;
    VerilatorRunner runner_;
    market::ModelParameters active_parameters_{};
    std::chrono::steady_clock::duration backpressure_timeout_;
    std::atomic_bool stop_requested_{false};
    std::atomic_bool stream_drained_{false};
    VerilatorWorkerMetrics metrics_{};
    std::function<void(const market::ModelParameters&)> model_applied_;
};

}  // namespace market_engine::verilator
