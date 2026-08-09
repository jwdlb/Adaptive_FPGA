#include "gpu/gpu_worker.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace market_engine::gpu {

GpuWorker::GpuWorker(GpuModel& model,
                     app::SpscRingBuffer<verilator::RtlStreamResult>& result_ring,
                     ModelUpdateMailbox& update_mailbox,
                     const std::size_t batch_rows,
                     const std::uint64_t first_update_version)
    : model_(model), result_ring_(result_ring), update_mailbox_(update_mailbox),
      batch_rows_(batch_rows), next_update_version_(first_update_version) {
    if (batch_rows_ == 0U) throw std::invalid_argument("GPU worker batch size must be positive");
    if (next_update_version_ == 0U) throw std::invalid_argument("first GPU model-update version must be positive");
}

void GpuWorker::begin_mapped_batch() {
    if (!mapped_values_.empty() || mapped_row_count_ != 0U) {
        throw std::logic_error("GPU worker already owns a mapped feature batch");
    }
    mapped_values_ = model_.map_stream_feature_rows(batch_rows_);
    const std::size_t expected_values = batch_rows_ * market::FeatureVector::kFeatureCount;
    if (mapped_values_.size() != expected_values) {
        throw std::runtime_error("GPU model mapped the wrong number of streaming feature values");
    }
}

void GpuWorker::copy_valid_row(const verilator::RtlStreamResult& result) {
    if (!result.features.valid) {
        ++metrics_.invalid_feature_results_discarded;
        return;
    }
    if (mapped_values_.empty()) begin_mapped_batch();

    const std::size_t offset = mapped_row_count_ * market::FeatureVector::kFeatureCount;
    std::copy(result.features.values.begin(), result.features.values.end(), mapped_values_.begin() + offset);
    ++mapped_row_count_;
    ++metrics_.valid_feature_rows_copied;

    if (mapped_row_count_ == batch_rows_) {
        // This unmaps the OpenCL-visible input before the GPU uses it, starts the
        // deterministic Phase 6 update kernel, and returns immediately. The RTL
        // worker remains independent because this separate thread owns the wait.
        model_.submit_phase6_model_update(next_update_version_);
        mapped_values_ = {};
        mapped_row_count_ = 0U;
        model_update_in_flight_ = true;
        ++metrics_.batches_submitted;
    }
}

void GpuWorker::poll_model_update() {
    if (!model_update_in_flight_) return;
    const std::optional<ModelUpdate> update = model_.poll_phase6_model_update();
    if (!update) return;

    // The mailbox stores only the newest complete replacement. publish() checks
    // the version and BUY/SELL ordering before another thread can observe it.
    update_mailbox_.publish(*update);
    next_update_version_ = update->update_version + 1U;
    model_update_in_flight_ = false;
    ++metrics_.model_updates_published;
}

void GpuWorker::run() {
    while (!stop_requested_.load(std::memory_order_relaxed) || model_update_in_flight_) {
        bool made_progress = false;

        if (model_update_in_flight_) {
            const std::size_t updates_before = metrics_.model_updates_published;
            poll_model_update();
            made_progress = metrics_.model_updates_published != updates_before;
        }

        if (stop_requested_.load(std::memory_order_relaxed)) {
            // Do not submit a short trailing training batch. Its mapped memory is
            // returned cleanly, while a full already-submitted batch still polls
            // above until it publishes its ModelUpdate.
            if (!mapped_values_.empty()) {
                model_.discard_stream_feature_rows();
                mapped_values_ = {};
                mapped_row_count_ = 0U;
            }
            if (!model_update_in_flight_) break;
        } else if (!model_update_in_flight_) {
            if (const verilator::RtlStreamResult* result = result_ring_.try_begin_pop(); result != nullptr) {
                // Copy the eight values while this slot is owned by the consumer,
                // then release it immediately. OpenCL never holds the SPSC slot.
                copy_valid_row(*result);
                result_ring_.finish_pop();
                ++metrics_.rtl_results_consumed;
                made_progress = true;
            }
        }

        if (!made_progress) std::this_thread::yield();
    }
}

void GpuWorker::request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_relaxed);
}

GpuWorkerMetrics GpuWorker::metrics() const noexcept { return metrics_; }

}  // namespace market_engine::gpu
