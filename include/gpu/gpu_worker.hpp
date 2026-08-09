// This file declares the separate GPU-side streaming worker. It is the sole
// consumer of RTL results from the SPSC ring: valid eight-feature rows are
// copied directly into mapped OpenCL `[N][8]` memory, then a completed GPU
// ModelUpdate is published to the newest-value mailbox for the RTL worker.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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
};

class GpuWorker {
public:
    GpuWorker(GpuModel& model,
              app::SpscRingBuffer<verilator::RtlStreamResult>& result_ring,
              ModelUpdateMailbox& update_mailbox,
              std::size_t batch_rows,
              std::uint64_t first_update_version = 2U);

    GpuWorker(const GpuWorker&) = delete;
    GpuWorker& operator=(const GpuWorker&) = delete;

    // Run on the dedicated GPU-side thread. It never accesses Verilator or RTL
    // state. On an orderly stop, a partial mapped batch is discarded, while an
    // already submitted GPU update is allowed to finish and reach the mailbox.
    void run();
    void request_stop() noexcept;

    // Read only after run() has returned or the GPU thread has joined.
    [[nodiscard]] GpuWorkerMetrics metrics() const noexcept;

private:
    void begin_mapped_batch();
    void copy_valid_row(const verilator::RtlStreamResult& result);
    void poll_model_update();

    GpuModel& model_;
    app::SpscRingBuffer<verilator::RtlStreamResult>& result_ring_;
    ModelUpdateMailbox& update_mailbox_;
    const std::size_t batch_rows_;
    std::uint64_t next_update_version_;
    std::span<std::int32_t> mapped_values_{};
    std::size_t mapped_row_count_{};
    bool model_update_in_flight_{false};
    std::atomic_bool stop_requested_{false};
    GpuWorkerMetrics metrics_{};
};

}  // namespace market_engine::gpu
