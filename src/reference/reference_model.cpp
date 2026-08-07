#include "reference/reference_model.hpp"

#include <cstdlib>

#include "market/fixed_point.hpp"

namespace market_engine::reference {

ReferenceModel::ReferenceModel(const app::Config& config) : book_(config.allow_crossed_books) {
    // Creates the model's parameters: eight model weights, buy/sell thresholds,
    // model version, and update count. The default values initialise every field to zero.
    parameters_.buy_threshold = market::fixed_point::from_double(config.buy_threshold);
    parameters_.sell_threshold = market::fixed_point::from_double(config.sell_threshold);
    parameters_.model_version = 1U;
}

market::ApplyResult ReferenceModel::process(const market::MarketEvent& event, const std::uint64_t event_index) {
    // Applies the current market event to the order book and returns its error and flow deltas.
    const market::ApplyResult result = book_.apply(event);
    if (result.error != market::BookError::None) return result;

    // The midpoint exists only when both best bid and best ask exist.
    const auto midpoint = book_.midpoint_ticks();
    // Calculate the absolute midpoint change only when both the current and prior midpoints exist.
    const auto midpoint_change = midpoint && prior_midpoint_
        ? std::llabs(static_cast<long long>(*midpoint) - *prior_midpoint_)
        : 0LL;

    // The window keeps only the most recent 64 order-flow, trade-flow, and midpoint-movement entries.
    window_.push(result.order_flow_delta, result.trade_flow_delta, midpoint_change);
    if (midpoint) prior_midpoint_ = midpoint;
    features_ = market::calculate_features(book_, window_);
    signal_ = market::evaluate_strategy(event.timestamp_ns, event_index, features_, parameters_);
    return result;
}

}  // namespace market_engine::reference
