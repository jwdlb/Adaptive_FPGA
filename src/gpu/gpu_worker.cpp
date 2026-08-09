#include "gpu/gpu_worker.hpp"

#include <algorithm>
#include <stdexcept>
#include <thread>

#include "market/fixed_point.hpp"

namespace market_engine::gpu {

GpuWorker::GpuWorker(GpuModel& model,
                     app::SpscRingBuffer<verilator::RtlStreamResult>& result_ring,
                     ModelUpdateMailbox& update_mailbox,
                     const std::size_t batch_rows,
                     const std::uint64_t first_update_version,
                     const std::uint64_t label_horizon_events,
                     const std::int32_t minimum_profit_ticks,
                     const std::int32_t learning_rate_q16, const std::int32_t l2_q16)
    : model_(model), result_ring_(result_ring), update_mailbox_(update_mailbox),
      batch_rows_(batch_rows), next_update_version_(first_update_version),
      label_horizon_events_(label_horizon_events), minimum_profit_ticks_(minimum_profit_ticks),
      learning_rate_q16_(learning_rate_q16), l2_q16_(l2_q16) {
    if (batch_rows_ == 0U || label_horizon_events_ == 0U || minimum_profit_ticks_ <= 0 || learning_rate_q16_ <= 0 || l2_q16_ < 0) {
        throw std::invalid_argument("GPU training batch, horizon, profit threshold, learning rate, and L2 regularisation are invalid");
    }
}

void GpuWorker::begin_mapped_batch() {
    if (!mapped_values_.empty() || !mapped_labels_.empty() || mapped_row_count_ != 0U) {
        throw std::logic_error("GPU worker already owns a mapped training batch");
    }
    const MappedTrainingBatch batch = model_.map_training_batch(batch_rows_);
    mapped_values_ = batch.features;
    mapped_labels_ = batch.labels;
    if (mapped_values_.size() != batch_rows_ * market::FeatureVector::kFeatureCount ||
        mapped_labels_.size() != batch_rows_) {
        throw std::runtime_error("GPU model mapped the wrong training-batch shape");
    }
}

void GpuWorker::add_labelled_row(const verilator::RtlStreamResult& entry,
                                 const verilator::RtlStreamResult& future) {
    if (!entry.features.valid || entry.best_bid_price_ticks <= 0 || entry.best_ask_price_ticks <= 0 ||
        future.best_bid_price_ticks <= 0 || future.best_ask_price_ticks <= 0 ||
        entry.best_bid_quantity == 0U || entry.best_ask_quantity == 0U ||
        future.best_bid_quantity == 0U || future.best_ask_quantity == 0U) {
        ++metrics_.invalid_feature_results_discarded;
        return;
    }
    if (mapped_values_.empty()) begin_mapped_batch();

    const std::size_t offset = mapped_row_count_ * market::FeatureVector::kFeatureCount;
    std::copy(entry.features.values.begin(), entry.features.values.end(), mapped_values_.begin() + offset);
    const std::int64_t long_profit = static_cast<std::int64_t>(future.best_bid_price_ticks) - entry.best_ask_price_ticks;
    const std::int64_t short_profit = static_cast<std::int64_t>(entry.best_bid_price_ticks) - future.best_ask_price_ticks;
    mapped_labels_[mapped_row_count_] =
        long_profit >= minimum_profit_ticks_ && long_profit > short_profit ? market::fixed_point::kOne :
        short_profit >= minimum_profit_ticks_ && short_profit > long_profit ? -market::fixed_point::kOne : 0;
    ++mapped_row_count_;
    ++metrics_.valid_feature_rows_copied;
    ++metrics_.labelled_rows_created;

    if (mapped_row_count_ == batch_rows_) {
        model_.submit_training_batch(next_update_version_, learning_rate_q16_, l2_q16_);
        mapped_values_ = {};
        mapped_labels_ = {};
        mapped_row_count_ = 0U;
        model_update_in_flight_ = true;
        ++metrics_.batches_submitted;
    }
}

void GpuWorker::consume_result(const verilator::RtlStreamResult& result) {
    while (!pending_labels_.empty() &&
           result.event_index >= pending_labels_.front().event_index + label_horizon_events_) {
        add_labelled_row(pending_labels_.front(), result);
        pending_labels_.pop_front();
    }
    if (result.error == market::BookError::None) pending_labels_.push_back(result);
}

void GpuWorker::poll_model_update() {
    if (!model_update_in_flight_) return;
    const std::optional<ModelUpdate> update = model_.poll_training_update();
    if (!update) return;
    update_mailbox_.publish(*update);
    next_update_version_ = update->update_version + 1U;
    model_update_in_flight_ = false;
    ++metrics_.model_updates_published;
}

void GpuWorker::run() {
    while (true) {
        bool made_progress = false;
        if (model_update_in_flight_) {
            const std::size_t before = metrics_.model_updates_published;
            poll_model_update();
            made_progress = metrics_.model_updates_published != before;
        }

        if (stop_requested_.load(std::memory_order_relaxed)) {
            if (!mapped_values_.empty()) {
                model_.discard_training_batch();
                mapped_values_ = {}; mapped_labels_ = {}; mapped_row_count_ = 0U;
            }
            pending_labels_.clear();
            if (!model_update_in_flight_) break;
        } else if (!model_update_in_flight_) {
            if (const verilator::RtlStreamResult* result = result_ring_.try_begin_pop(); result != nullptr) {
                // This is the only time the SPSC slot is held. GPU execution
                // begins later from mapped OpenCL memory, never from the ring.
                consume_result(*result);
                result_ring_.finish_pop();
                ++metrics_.rtl_results_consumed;
                made_progress = true;
            }
        }

        if (input_complete_.load(std::memory_order_relaxed) && result_ring_.empty() && !model_update_in_flight_) {
            if (!mapped_values_.empty()) {
                model_.discard_training_batch();
                mapped_values_ = {}; mapped_labels_ = {}; mapped_row_count_ = 0U;
            }
            pending_labels_.clear(); // Last horizon rows have no future outcome.
            break;
        }
        if (!made_progress) std::this_thread::yield();
    }
}

void GpuWorker::request_input_complete() noexcept { input_complete_.store(true, std::memory_order_relaxed); }
void GpuWorker::request_stop() noexcept { stop_requested_.store(true, std::memory_order_relaxed); }
GpuWorkerMetrics GpuWorker::metrics() const noexcept { return metrics_; }

}  // namespace market_engine::gpu
