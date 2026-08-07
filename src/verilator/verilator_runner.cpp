#include "verilator/verilator_runner.hpp"

#include <array>
#include <chrono>
#include <stdexcept>
#include <string>

#include "Vmarket_pipeline_cpp_wrapper.h"
#include "verilated.h"

namespace market_engine::verilator {
namespace {

constexpr std::size_t kBookDepth = market::kBookDepth;
constexpr std::size_t kFeatureCount = market::FeatureVector::kFeatureCount;
constexpr std::size_t kCycleLimit = 64U;

// Split one packed RTL book level into its price and quantity fields.
[[nodiscard]] market::PriceLevel decode_level(const std::uint64_t packed) noexcept {
    return {.price_ticks = static_cast<std::int32_t>(packed >> 32U), .quantity = static_cast<std::uint32_t>(packed)};
}

}  // namespace

class VerilatorRunner::Impl {
public:
    // Set up the simulated RTL pipeline, then reset it into a known state.
    explicit Impl(const std::uint32_t clock_period_ns) : clock_period_ns_(clock_period_ns), dut_(&context_) {
        if (clock_period_ns_ == 0U) throw std::invalid_argument("Verilator clock period must be non-zero");
        reset();
    }

    // Let the generated RTL model perform its final simulation cleanup.
    ~Impl() { dut_.final(); }


    // For the default 10 ns period, the sequence is:
    // time 0 ns:   clk = 0, evaluate
    // time 5 ns:   clk = 1, evaluate  ← state registers update here
    // time 10 ns:  one simulated cycle complete

    // dut_ is the Verilator-generated C++ object representing the SystemVerilog top module. Assignments such as dut_.clk = 1 drive input pins; dut_.eval() asks Verilator to evaluate the model after the pin change.
    void tick() {   // simulates exactly one full RTL clock period
        dut_.clk = 0;                              // Falling/low phase
        dut_.eval();                               // Let combinational logic settle
        context_.timeInc(clock_period_ns_ / 2U);   // Advance simulated time by half a period

        dut_.clk = 1;                              // Rising edge
        dut_.eval();                               // Sequential always_ff blocks react here
        context_.timeInc(clock_period_ns_ - clock_period_ns_ / 2U);  // advances Verilator’s simulated clock to the end of the current clock period.

        ++metrics_.cycles;  // increments the runner’s count of complete simulated clock cycles.  It is a host-side measurement only. It does not drive an RTL signal or alter hardware behavior.
    }
    
    // reset() forces the RTL into a known, inactive state: dut_.rst_n = 0;
    void reset() {
        dut_.rst_n = 0;  // The _n suffix means active-low reset: 0 means “reset asserted”; 1 means “normal operation.”

        // It also clears every input pin controlled by the runner:
        dut_.in_valid = 0;    // no event is being submitted
        dut_.timestamp_ns = 0;
        dut_.event_type = 0;
        dut_.side = 0;
        dut_.price_ticks = 0;
        dut_.quantity = 0;
        dut_.param_write_valid = 0;  // no parameter write
        dut_.param_write_index = 0;
        dut_.param_write_value = 0;
        dut_.param_write_model_version = 0;
        dut_.param_commit = 0;   //  // no parameter-bank activation request

        // This holds reset active across two rising edges. The RTL’s sequential blocks get at least two chances to execute their if (!rst_n) branches and initialise all state.
        tick();
        tick();

        // Releasing reset
        // This takes the design through one normal rising edge after reset release. Now the order book/pipeline has an opportunity to enter its initial idle/ready state.
        dut_.rst_n = 1;
        tick();

        // This is a startup health check. A correctly reset pipeline should be willing to accept an input event. If not, simulation probably has a reset or RTL state-machine bug, so the runner fails early rather than hanging later.
        // read_snapshot() captures the empty post-reset book/model outputs as the initial value returned by latest().
        if (!dut_.in_ready) throw std::runtime_error("RTL pipeline did not become ready after reset");
        latest_ = read_snapshot();
    }

    // Prevents the runner from violating the RTL’s valid/ready protocol
    void wait_until_ready() {
        // The RTL sets in_ready = 1 when it can accept a new event. If it is still handling a previous event, the runner waits and keeps supplying clocks.
        // When in_ready becomes 1, the function returns. The caller can then safely drive an event and assert in_valid.
        for (std::size_t cycles = 0; !dut_.in_ready && cycles < kCycleLimit; ++cycles) tick();

        // The kCycleLimit limit is 64 cycles, so it will not wait forever:
        if (!dut_.in_ready) throw std::runtime_error("RTL pipeline did not become ready within the cycle limit");   // Converts a stuck pipeline into a clear error instead of an infinite simulation loop.
    }

    // send one market event and wait for its result.
    RtlSnapshot process(const market::MarketEvent& event) {
        const auto started = std::chrono::steady_clock::now();   // 
        wait_until_ready();   // Waits until dut_.in_ready is high. That ensures the RTL pipeline is idle and can accept a new event.

        // Copies the C++ MarketEvent fields onto the RTL input pins.
        // event.type and event.side are strongly typed C++ enums, while Verilator exposes hardware ports as ordinary integer-like C++ fields, hence the explicit casts.
        // price_ticks is logically signed in C++, but gets cast to uint32_t because the generated Verilator port is represented that way. 
        // This preserves the same 32-bit two’s-complement bit pattern. The RTL interprets those bits as signed where appropriate.
        dut_.timestamp_ns = event.timestamp_ns;
        dut_.event_type = static_cast<std::uint8_t>(event.type);
        dut_.side = static_cast<std::uint8_t>(event.side);
        dut_.price_ticks = static_cast<std::uint32_t>(event.price_ticks);
        dut_.quantity = event.quantity;

        // This performs a one-clock valid/ready transfer:
        // - Before rising edge:  event fields are driven; in_valid = 1; in_ready = 1
        // - At rising edge:      RTL accepts/latches the event
        // - Afterwards:          runner deasserts in_valid

        // The runner only keeps in_valid high for one cycle because it wants to submit exactly one event. Keeping it high could cause repeated acceptance when the pipeline becomes ready again.
        dut_.in_valid = 1;
        tick();
        dut_.in_valid = 0;

        // After acceptance, the pipeline may take several clocks to update the order book, calculate features, run the model, and publish results. This loop continues clocking until the RTL pulses event_done
        for (std::size_t cycles = 0; cycles < kCycleLimit; ++cycles) {
            tick();
            if (dut_.event_done) {
                // On completion:
                // - increment the event count;
                // - add the real elapsed execution time;
                // - decode RTL output pins into a C++ RtlSnapshot;
                // - cache it in latest_;
                // - return a copy to the caller.
                // The returned RtlSnapshot represents the state after this event, including the updated book, features, model decision, flow deltas, and any error.
                ++metrics_.completed_events;
                metrics_.wall_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                latest_ = read_snapshot();
                return latest_;
            }
        }
        // If event_done never arrives within 64 simulated cycles:
        throw std::runtime_error("RTL pipeline event timed out");
    }

    // load a complete model into the RTL’s shadow bank and activate it.
    // This sends ten values to the RTL:
    // - indices 0–7:  eight feature weights
    // - index 8:      buy threshold
    // - index 9:      sell threshold
    void write_model_parameters(const market::ModelParameters& parameters) {
        const std::uint64_t previous_updates = dut_.update_count;   // recording the current successful-commit count, this lets it later verify that the requested commit was accepted.
        for (std::size_t index = 0; index < kFeatureCount + 2U; ++index) { // iterating through the features
            // Wait until the event pipeline is idle before putting a parameter word on the interface. This prevents event submission and parameter activity from being interleaved by the runner.
            wait_until_ready();

            dut_.param_write_index = static_cast<std::uint8_t>(index);  // Selects the target shadow-register slot.
            // This nested conditional selects the value associated with that index:
            // - 0–7 -> parameters.weights[index]
            // - 8   -> parameters.buy_threshold
            // - 9   -> parameters.sell_threshold
            dut_.param_write_value = index < kFeatureCount ? parameters.weights[index]
                : (index == kFeatureCount ? parameters.buy_threshold : parameters.sell_threshold);
            // Sends the version label along with each write. The RTL keeps it as the version associated with the pending shadow-bank model.
            dut_.param_write_model_version = parameters.model_version;

            // This is the parameter-write equivalent of a one-cycle strobe:
            // Before rising edge: index/value/version valid
            // At rising edge:     RTL writes value into shadow register
            // Next cycle:         valid low; no duplicate write

            // The second tick() gives the simulation an inactive cycle between writes. Strictly speaking, the RTL can accept consecutive valid writes if designed for it, but this makes each transaction clean and simple to inspect in a waveform.
            dut_.param_write_valid = 1;
            tick();
            dut_.param_write_valid = 0;
            tick();
        }

        //    Committing the shadow bank
        // This sends a one-cycle commit pulse. The RTL will only honour it when:
        // - all ten shadow entries have been written;
        // - it is not evaluating an event;
        // - the pipeline is idle.
        // On acceptance, it copies all shadow values to the active bank together, changes model_version, increments update_count, and clears its “written” tracking bits for the next upload.
        wait_until_ready();
        dut_.param_commit = 1;
        tick();
        dut_.param_commit = 0;
        tick();

        // Captures the state after the attempted commit.
        latest_ = read_snapshot();

        // This validates that the commit really took effect: - model_version must match the model just uploaded; - update_count must advance by exactly one.
        if (latest_.signal.model_version != parameters.model_version || latest_.update_count != previous_updates + 1U) {
            throw std::runtime_error("RTL parameter commit did not become active");
        }
    }

    // Return one packed order-book level from the Verilated RTL output ports.
    //
    // The RTL exposes the ten bid and ten ask levels as separate flattened ports
    // (bid_level0 ... bid_level9 and ask_level0 ... ask_level9), rather than as
    // C++ arrays. This helper restores indexed access for the snapshot reader.
    //
    // `bid` selects the bid side when true, or the ask side when false.
    // `index` must be in the range 0–9; any other value is a programming error.
    [[nodiscard]] std::uint64_t packed_level(const bool bid, const std::size_t index) const {
        if (bid) {
            switch (index) {
                case 0: return dut_.bid_level0; case 1: return dut_.bid_level1;
                case 2: return dut_.bid_level2; case 3: return dut_.bid_level3;
                case 4: return dut_.bid_level4; case 5: return dut_.bid_level5;
                case 6: return dut_.bid_level6; case 7: return dut_.bid_level7;
                case 8: return dut_.bid_level8; case 9: return dut_.bid_level9;
                default: break;
            }
        } else {
            switch (index) {
                case 0: return dut_.ask_level0; case 1: return dut_.ask_level1;
                case 2: return dut_.ask_level2; case 3: return dut_.ask_level3;
                case 4: return dut_.ask_level4; case 5: return dut_.ask_level5;
                case 6: return dut_.ask_level6; case 7: return dut_.ask_level7;
                case 8: return dut_.ask_level8; case 9: return dut_.ask_level9;
                default: break;
            }
        }
        throw std::out_of_range("invalid RTL snapshot index");
    }


    // Return one feature value from the Verilated RTL output ports.
    //
    // The eight RTL feature outputs are flattened into feature0 ... feature7.
    // This helper maps a normal C++ index to the corresponding generated port,
    // allowing read_snapshot() to populate FeatureVector with a simple loop.
    //
    // `index` must be in the range 0–7; any other value is a programming error.
    [[nodiscard]] std::int32_t feature_value(const std::size_t index) const {
        switch (index) {
            case 0: return dut_.feature0; case 1: return dut_.feature1;
            case 2: return dut_.feature2; case 3: return dut_.feature3;
            case 4: return dut_.feature4; case 5: return dut_.feature5;
            case 6: return dut_.feature6; case 7: return dut_.feature7;
            default: throw std::out_of_range("invalid RTL feature index");
        }
    }


    // Decode the RTL's current output pins into the application's C++ snapshot type.
    //
    // This does not advance simulated time or change RTL state. It takes a
    // point-in-time copy of the current hardware-visible state: book levels,
    // calculated features, strategy signal, event result, flow deltas, and model
    // update count.
    //
    // Each packed 64-bit book-level port is converted back into PriceLevel
    // { price_ticks, quantity } before being stored in the C++ BookSnapshot.
    [[nodiscard]] RtlSnapshot read_snapshot() const {
        RtlSnapshot result{};
        // Decode all ten packed bid and ask price levels from their flattened RTL ports.
        for (std::size_t index = 0; index < kBookDepth; ++index) {
            result.book.bids[index] = decode_level(packed_level(true, index));
            result.book.asks[index] = decode_level(packed_level(false, index));
        }

        // Copy the eight flattened RTL feature outputs into the C++ feature array.
        for (std::size_t index = 0; index < kFeatureCount; ++index) result.features.values[index] = feature_value(index);
        result.features.valid = dut_.feature_valid;
        result.signal.timestamp_ns = dut_.signal_timestamp_ns;
        result.signal.event_index = dut_.signal_event_index;
        result.signal.score = dut_.score;
        result.signal.action = static_cast<market::Action>(dut_.action);
        result.signal.valid = dut_.signal_valid;
        result.signal.model_version = dut_.model_version;
        result.error = static_cast<market::BookError>(dut_.event_error);
        result.order_flow_delta = dut_.order_flow_delta;
        result.trade_flow_delta = dut_.trade_flow_delta;
        result.update_count = dut_.update_count;
        return result;
    }

    // Simulated duration of one complete RTL clock cycle.
    std::uint32_t clock_period_ns_;
    // Verilator-owned simulation state, including the current simulated time.
    VerilatedContext context_;
    // Generated C++ model of the RTL top-level wrapper; this runner drives its pins.
    Vmarket_pipeline_cpp_wrapper dut_;
    // Most recently decoded stable RTL state, exposed through latest().
    RtlSnapshot latest_{};
    // Cumulative host-side measurements for this runner instance.
    RunnerMetrics metrics_{};
};

// Create and reset a simulated RTL pipeline with the requested clock period.
VerilatorRunner::VerilatorRunner(const std::uint32_t clock_period_ns) : impl_(std::make_unique<Impl>(clock_period_ns)) {}
// Clean up the simulated RTL pipeline when the runner goes out of scope.
VerilatorRunner::~VerilatorRunner() = default;
// Move ownership of a simulated RTL pipeline into this runner.
VerilatorRunner::VerilatorRunner(VerilatorRunner&&) noexcept = default;
// Replace this runner's simulated RTL pipeline with one from another runner.
VerilatorRunner& VerilatorRunner::operator=(VerilatorRunner&&) noexcept = default;
// Return the simulated RTL pipeline to its initial empty state.
void VerilatorRunner::reset() { impl_->reset(); }
// Send one market event to the RTL and return its completed result.
RtlSnapshot VerilatorRunner::process(const market::MarketEvent& event) { return impl_->process(event); }
// Load a complete set of model parameters and make it active in the RTL.
void VerilatorRunner::write_model_parameters(const market::ModelParameters& parameters) { impl_->write_model_parameters(parameters); }
// Return the most recently captured result without advancing the simulation.
const RtlSnapshot& VerilatorRunner::latest() const noexcept { return impl_->latest_; }
// Return cumulative counts and timings collected while running the simulation.
RunnerMetrics VerilatorRunner::metrics() const noexcept { return impl_->metrics_; }

}  // namespace market_engine::verilator
