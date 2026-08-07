#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "app/config.hpp"
#include "market/order_book.hpp"

namespace market_engine::app {

// Results from a completed reference replay or its first encountered error.
struct ReplayResult {
    std::size_t processed_events{};
    double elapsed_seconds{};
    std::optional<market::BookError> error;
    std::optional<std::size_t> failure_index;
    market::BookSnapshot final_book{};
    market::FeatureVector final_features{};
    market::Signal final_signal{};
    market::ModelParameters final_parameters{};
};

// Coordinates replay modes. Future RTL and GPU work enters here; the reference mode
// remains available as the deterministic correctness oracle.
class ReplayCoordinator {
public:
    explicit ReplayCoordinator(const Config& config) : config_(config) {}

    [[nodiscard]] ReplayResult run_reference(std::span<const market::MarketEvent> events,
                                             std::optional<std::uint64_t> event_limit) const;

private:
    Config config_;
};

}  // namespace market_engine::app
