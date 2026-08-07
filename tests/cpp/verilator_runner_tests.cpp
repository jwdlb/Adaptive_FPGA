// End-to-end test for Adaptive_FPGA's C++ to RTL simulation boundary.
//
// Adaptive_FPGA processes market events in an FPGA-style RTL pipeline. This
// program uses VerilatorRunner to load a simple trading model, send events to
// the simulated pipeline, and check its order-book, feature, signal, error,
// and metrics outputs.
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>

#include "market/fixed_point.hpp"
#include "verilator/verilator_runner.hpp"

namespace {

using market_engine::market::Action;
using market_engine::market::BookError;
using market_engine::market::EventType;
using market_engine::market::MarketEvent;
using market_engine::market::ModelParameters;
using market_engine::market::Side;
using market_engine::verilator::VerilatorRunner;

// Stop the test immediately and report a helpful reason when a check fails.
void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

}  // namespace

int main() {
    try {
        // Creating the runner also resets the simulated RTL pipeline.
        VerilatorRunner runner;
        // A reset pipeline must expose an empty order book.
        require(runner.latest().book.bids[0].quantity == 0U, "reset did not clear the bid book");
        require(runner.latest().book.asks[0].quantity == 0U, "reset did not clear the ask book");

        // Load a deliberately simple model: only feature 7 affects the score.
        ModelParameters parameters{};
        parameters.weights[7] = market_engine::market::fixed_point::kOne;
        parameters.buy_threshold = 0;
        parameters.sell_threshold = -market_engine::market::fixed_point::kOne;
        parameters.model_version = 1U;

        // Write the model to the RTL and check that its atomic commit succeeded.
        runner.write_model_parameters(parameters);
        require(runner.latest().signal.model_version == 1U, "model version did not commit");
        require(runner.latest().update_count == 1U, "model update counter did not increment");

        // Add a bid. A one-sided book cannot yet produce valid features or a signal.
        const auto bid = runner.process({.timestamp_ns = 1U, .type = EventType::Add, .side = Side::Bid,
                                         .price_ticks = 100, .quantity = 20U});
        require(bid.error == BookError::None, "bid event failed");
        require(!bid.features.valid && bid.signal.action == Action::Hold, "one-sided book signal is wrong");

        // Add an ask. With both sides present, features and the model signal become valid.
        const auto ask = runner.process({.timestamp_ns = 2U, .type = EventType::Add, .side = Side::Ask,
                                         .price_ticks = 102, .quantity = 10U});
        require(ask.error == BookError::None, "ask event failed");
        require(ask.features.valid && ask.signal.valid, "valid book did not produce a valid feature/signal snapshot");
        // The selected parameters make this valid book produce a score of +1.0 and Buy.
        require(ask.signal.action == Action::Buy, "bias-only parameters did not produce Buy");
        require(ask.signal.score == market_engine::market::fixed_point::kOne, "unexpected bias-only score");
        require(ask.signal.timestamp_ns == 2U && ask.signal.event_index == 1U, "signal metadata is wrong");
        require(ask.book.bids[0].price_ticks == 100 && ask.book.asks[0].price_ticks == 102, "snapshot is wrong");

        // Invalid input must report an error without changing the existing book.
        const auto invalid = runner.process({.timestamp_ns = 3U, .type = EventType::Add, .side = Side::Bid,
                                             .price_ticks = 0, .quantity = 1U});
        require(invalid.error == BookError::InvalidEvent, "invalid event was not reported");
        require(invalid.book.bids[0].price_ticks == 100, "invalid event changed the book");

        // Confirm that the runner recorded all three submitted events and simulation work.
        const auto metrics = runner.metrics();
        require(metrics.completed_events == 3U && metrics.cycles > 0U && metrics.wall_seconds >= 0.0,
                "runner metrics are incomplete");
        std::cout << "VerilatorRunner tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "VerilatorRunner test failed: " << error.what() << '\n';
        return 1;
    }
}
