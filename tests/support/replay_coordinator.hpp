// Test-only replay harness. It drives the C++ reference model and the RTL with
// the same recorded events, then reports their first difference. The normal
// application path is app/live_coordinator.hpp and has no dependency on this.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "app/config.hpp"
#include "market/order_book.hpp"

namespace market_engine::gpu {
class GpuModel;
}

namespace market_engine::test_support {

// Results from a completed reference replay or its first encountered error.
struct ReplayResult {
    std::size_t processed_events{};
    double elapsed_seconds{};
    std::optional<market::BookError> error;
    std::optional<std::size_t> failure_index;
    std::optional<std::string> divergence_message;
    market::BookSnapshot final_book{};
    market::FeatureVector final_features{};
    market::Signal final_signal{};
    market::ModelParameters final_parameters{};
    std::uint64_t rtl_cycles{};
    double rtl_wall_seconds{};
    std::size_t gpu_feature_batches_submitted{};
    std::size_t gpu_feature_uploads_completed{};
};

// Test-only coordinator for deterministic C++ reference and RTL comparisons.
class ReplayCoordinator {
public:
    explicit ReplayCoordinator(const app::Config& config) : config_(config) {}

    [[nodiscard]] ReplayResult run_reference(std::span<const market::MarketEvent> events,
                                             std::optional<std::uint64_t> event_limit) const;
    [[nodiscard]] ReplayResult run_verilator_check(std::span<const market::MarketEvent> events,
                                                    std::optional<std::uint64_t> event_limit,
                                                    gpu::GpuModel* gpu_model = nullptr) const;

private:
    app::Config config_;
};

}  // namespace market_engine::test_support
