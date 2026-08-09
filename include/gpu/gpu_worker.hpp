// This file declares the separate GPU-side streaming worker. It is the sole
// consumer of RTL results from the SPSC ring: valid eight-feature rows are
// copied directly into mapped OpenCL `[N][8]` memory, then a completed GPU
// ModelUpdate is published to the newest-value mailbox for the RTL worker.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>

#include "app/spsc_ring_buffer.hpp"
#include "gpu/gpu_model.hpp"
#include "gpu/model_update_mailbox.hpp"
#include "verilator/rtl_stream.hpp"

namespace market_engine::gpu {

struct GpuWorkerMetrics {
    std::size_t rtl_results_consumed{};
    std::size_t invalid_feature_results_discarded{};
    std::size_t valid_feature_rows_copied{};
    std::size_t batches_submitted{};
    std::size_t model_updates_published{};
    std::size_t labelled_rows_created{};
    std::int64_t latest_squared_error_sum_q16{};
    std::uint64_t latest_correct_predictions{};
    std::uint64_t latest_training_rows{};
    double latest_kernel_ms{};
    double latest_upload_ms{};
    double latest_readback_ms{};
    double latest_update_latency_ms{};
};

class GpuWorker {
public:
    GpuWorker(GpuModel& model,
              app::SpscRingBuffer<verilator::RtlStreamResult>& result_ring,
              ModelUpdateMailbox& update_mailbox,
              std::size_t batch_rows,
              std::uint64_t first_update_version = 2U,
              std::uint64_t label_horizon_events = 100U,
              std::int32_t minimum_profit_ticks = 1,
              std::int32_t learning_rate_q16 = 66,
              std::int32_t l2_q16 = 0);

    GpuWorker(const GpuWorker&) = delete;
    GpuWorker& operator=(const GpuWorker&) = delete;

    // Run on the dedicated GPU-side thread. It never accesses Verilator or RTL
    // state. On an orderly stop, a partial mapped batch is discarded, while an
    // already submitted GPU update is allowed to finish and reach the mailbox.
    void run();
    // Tell the worker that the RTL producer has published its final result. It
    // then drains every remaining ring value, completes an already submitted GPU
    // update, discards only a short final batch, and returns from run().
    void request_input_complete() noexcept;
    void request_stop() noexcept;

    // Read only after run() has returned or the GPU thread has joined.
    [[nodiscard]] GpuWorkerMetrics metrics() const noexcept;

private:
    void begin_mapped_batch();
    void consume_result(const verilator::RtlStreamResult& result);
    void add_labelled_row(const verilator::RtlStreamResult& entry,
                          const verilator::RtlStreamResult& future);
    void poll_model_update();

    GpuModel& model_;
    app::SpscRingBuffer<verilator::RtlStreamResult>& result_ring_;
    ModelUpdateMailbox& update_mailbox_;
    const std::size_t batch_rows_;
    std::uint64_t next_update_version_;
    const std::uint64_t label_horizon_events_;
    const std::int32_t minimum_profit_ticks_;
    const std::int32_t learning_rate_q16_;
    const std::int32_t l2_q16_;
    std::span<std::int32_t> mapped_values_{};
    std::span<std::int32_t> mapped_labels_{};
    std::size_t mapped_row_count_{};
    std::deque<verilator::RtlStreamResult> pending_labels_;
    bool model_update_in_flight_{false};
    std::atomic_bool input_complete_{false};
    std::atomic_bool stop_requested_{false};
    GpuWorkerMetrics metrics_{};
};

}  // namespace market_engine::gpu
