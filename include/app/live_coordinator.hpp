// This file declares the normal RTL application path. LiveCoordinator accepts
// market events from any source, starts the dedicated RTL worker, and optionally
// starts the separate GPU worker. It does not run the C++ reference model or
// compare two implementations.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "app/config.hpp"
#include "market/order_book.hpp"
#include "verilator/verilator_runner.hpp"

namespace market_engine::gpu {
class GpuModel;
}

namespace market_engine::app {

// Results from normal RTL application processing. Unlike ReplayResult, this is
// the actual RTL result, not a C++/RTL comparison report.
struct LiveResult {
    std::size_t processed_events{};
    double elapsed_seconds{};
    std::optional<market::BookError> error;
    std::optional<std::size_t> failure_index;
    verilator::RtlSnapshot final_rtl{};
    market::ModelParameters active_parameters{};
    std::uint64_t rtl_cycles{};
    double rtl_wall_seconds{};
    std::size_t rtl_stream_results_published{};
    std::size_t gpu_rtl_results_consumed{};
    std::size_t gpu_valid_feature_rows_copied{};
    std::size_t gpu_batches_submitted{};
    std::size_t gpu_model_updates_published{};
    std::size_t rtl_model_updates_applied{};
};

// Drives the operational path: event source -> dedicated RTL worker -> optional
// separate GPU worker -> newest-model mailbox -> RTL worker.
// Today the RTL is simulated through Verilator; a future physical-FPGA runner
// can provide the same event/result boundary without changing this coordinator.
class LiveCoordinator {
public:
    explicit LiveCoordinator(const Config& config) : config_(config) {}

    [[nodiscard]] LiveResult run(std::span<const market::MarketEvent> events,
                                 std::optional<std::uint64_t> event_limit,
                                 gpu::GpuModel* gpu_model = nullptr) const;

private:
    Config config_;
};

}  // namespace market_engine::app
