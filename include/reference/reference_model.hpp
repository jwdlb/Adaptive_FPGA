#pragma once

#include <cstdint>
#include <optional>

#include "app/config.hpp"
#include "market/order_book.hpp"

namespace market_engine::reference {

// The deterministic C++ answer key used to check the RTL replay path.
class ReferenceModel {
public:
    explicit ReferenceModel(const app::Config& config);

    [[nodiscard]] market::ApplyResult process(const market::MarketEvent& event, std::uint64_t event_index);
    [[nodiscard]] const market::BookSnapshot& book() const noexcept { return book_.snapshot(); }
    [[nodiscard]] const market::FeatureVector& features() const noexcept { return features_; }
    [[nodiscard]] const market::Signal& signal() const noexcept { return signal_; }
    [[nodiscard]] const market::ModelParameters& parameters() const noexcept { return parameters_; }

private:
    market::OrderBook book_;
    market::FeatureWindow window_;
    market::ModelParameters parameters_{};
    std::optional<std::int32_t> prior_midpoint_;
    market::FeatureVector features_{};
    market::Signal signal_{};
};

}  // namespace market_engine::reference
