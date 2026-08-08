#include "gpu/gpu_protocol.hpp"

#include <stdexcept>

namespace market_engine::gpu {

std::optional<FeatureBatch> FeatureBatchCollector::add(const FeatureSnapshot& snapshot) {
    if (!snapshot.valid) return std::nullopt;

    batch_[size_] = snapshot.features;
    ++size_;
    if (size_ != kFeatureBatchTimeSteps) return std::nullopt;

    FeatureBatch completed = batch_;
    reset();
    return completed;
}

void FeatureBatchCollector::reset() noexcept {
    batch_ = {};
    size_ = 0U;
}

void validate_model_update(const ModelUpdate& update, std::uint64_t active_model_version) {
    if (update.update_version <= active_model_version) {
        throw std::invalid_argument("GPU model update version must be newer than the active RTL model version");
    }
    if (update.buy_threshold <= update.sell_threshold) {
        throw std::invalid_argument("GPU BUY threshold must be strictly greater than its SELL threshold");
    }
}

market::ModelParameters model_parameters_from_update(const ModelUpdate& update, std::uint64_t active_model_version) {
    validate_model_update(update, active_model_version);
    return {.weights = update.weights,
            .buy_threshold = update.buy_threshold,
            .sell_threshold = update.sell_threshold,
            .model_version = update.update_version};
}

}  // namespace market_engine::gpu
