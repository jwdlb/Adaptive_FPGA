#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "market/event.hpp"
#include "market/order_book.hpp"

namespace market_engine::verilator {

// Snapshot returned after the RTL completes one event OR an atomic parameter commit (update to one of the rtl model parameters / weights).
struct RtlSnapshot {
    market::BookSnapshot book{};  // ten bid and ten ask levels
    market::FeatureVector features{};  // the eight fixed-point model inputs
    market::Signal signal{};   // model score, action (Hold/Buy/Sell), timestamp, model version, etc.
    market::BookError error{market::BookError::None};   // whether the order-book operation was rejected
    std::int64_t order_flow_delta{};   // signed changes from the latest event
    std::int64_t trade_flow_delta{};   // signed changes from the latest event
    std::uint64_t update_count{};   // how many parameter commits the RTL has accepted
};

// Cumulative execution measurements collected by the simulation runner.
// This is deliberately separate from the market result. It records simulation-side observability:
struct RunnerMetrics {
    std::uint64_t cycles{};   // total clock cycles ticked by the runner
    std::uint64_t completed_events{};   // number of events that reached the RTL’s completion signal
    double wall_seconds{};   // real host time spent executing process() calls
};

// Owns a Verilated market_pipeline instance and provides the C++ side of its
// valid/ready and atomic-parameter-update interfaces.
class VerilatorRunner {
public:
    explicit VerilatorRunner(std::uint32_t clock_period_ns = 10U);  // Constructs and resets a simulation with a nominal 10 ns clock by default. The period controls simulated time; it is not a claim that the host process will run at FPGA speed.
    ~VerilatorRunner();
    // The class is movable but not copyable:        A simulated DUT is stateful and uniquely owned, so copying it would be ambiguous and expensive. Moving safely transfers that ownership.
    VerilatorRunner(VerilatorRunner&&) noexcept;
    VerilatorRunner& operator=(VerilatorRunner&&) noexcept;
    VerilatorRunner(const VerilatorRunner&) = delete;
    VerilatorRunner& operator=(const VerilatorRunner&) = delete;

    void reset();   // Drives the RTL reset sequence and clears its inputs. Afterwards the RTL must indicate it is ready to accept input.
    [[nodiscard]] RtlSnapshot process(const market::MarketEvent& event);   // Submits one normalized market event and waits for the hardware to finish it.
    void write_model_parameters(const market::ModelParameters& parameters);   // Writes eight weights plus buy/sell thresholds to the RTL’s shadow parameter bank, then asks it to commit them. The implementation checks that the new model version and update counter become active, which enforces the intended atomic-update behavior.
    [[nodiscard]] const RtlSnapshot& latest() const noexcept;   // latest() gives access to the most recently captured RTL result without another simulation step. Its reference stays valid until the runner updates or is destroyed. const noexcept because it's only read already-held state.
    [[nodiscard]] RunnerMetrics metrics() const noexcept;   // metrics() returns a small copy of the cumulative metrics, const noexcept because it's only read already-held state.

private:
    // This is the Pimpl (“pointer to implementation”) pattern. The header avoids including Verilator-generated headers such as Vmarket_pipeline_cpp_wrapper.h. 
    // That keeps consumers independent of Verilator internals and makes rebuilds cheaper.
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace market_engine::verilator
