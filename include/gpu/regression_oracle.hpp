#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "gpu/gpu_protocol.hpp"

namespace market_engine::gpu {

struct RegressionMetrics {
    std::int64_t squared_error_sum_q16{};
    std::uint64_t correct_predictions{};
    std::uint64_t rows{};
};

struct RegressionResult { ModelUpdate update{}; RegressionMetrics metrics{}; };

// Canonical fixed-point batch-gradient update used to verify the OpenCL learner.
[[nodiscard]] RegressionResult run_regression_oracle(std::span<const std::int32_t> features,
                                                       std::span<const std::int32_t> labels,
                                                       ModelUpdate initial,
                                                       std::int32_t learning_rate_q16,
                                                       std::int32_t l2_q16);
}  // namespace market_engine::gpu
