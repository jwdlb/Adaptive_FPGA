#include "gpu/regression_oracle.hpp"

#include <stdexcept>

namespace market_engine::gpu {
RegressionResult run_regression_oracle(const std::span<const std::int32_t> features,
                                       const std::span<const std::int32_t> labels, ModelUpdate initial,
                                       const std::int32_t learning_rate_q16, const std::int32_t l2_q16) {
    constexpr std::size_t kFeatures = market::FeatureVector::kFeatureCount;
    if (labels.empty() || features.size() != labels.size() * kFeatures || learning_rate_q16 <= 0 || l2_q16 < 0)
        throw std::invalid_argument("invalid fixed-point regression batch");
    RegressionResult result{.update = initial}; result.metrics.rows = labels.size();
    std::array<std::int64_t, kFeatures> gradients{};
    for (std::size_t row = 0; row < labels.size(); ++row) {
        std::int64_t score{};
        for (std::size_t feature = 0; feature < kFeatures; ++feature)
            score += (static_cast<std::int64_t>(initial.weights[feature]) * features[row * kFeatures + feature]) >> 16;
        const std::int64_t error = static_cast<std::int64_t>(labels[row]) - score;
        result.metrics.squared_error_sum_q16 += (error * error) >> 16;
        const auto prediction = score > 0 ? 65536 : (score < 0 ? -65536 : 0);
        result.metrics.correct_predictions += prediction == labels[row];
        for (std::size_t feature = 0; feature < kFeatures; ++feature)
            gradients[feature] += (error * features[row * kFeatures + feature]) >> 16;
    }
    for (std::size_t feature = 0; feature < kFeatures; ++feature) {
        const auto mean = gradients[feature] / static_cast<std::int64_t>(labels.size());
        const auto penalty = (static_cast<std::int64_t>(l2_q16) * initial.weights[feature]) >> 16;
        auto next = static_cast<std::int64_t>(initial.weights[feature]) +
                    ((static_cast<std::int64_t>(learning_rate_q16) * (mean - penalty)) >> 16);
        if (next > 2097152) next = 2097152;
        if (next < -2097152) next = -2097152;
        result.update.weights[feature] = static_cast<std::int32_t>(next);
    }
    if (result.update.buy_threshold <= result.update.sell_threshold) { result.update.buy_threshold = 16384; result.update.sell_threshold = -16384; }
    return result;
}
}  // namespace market_engine::gpu
