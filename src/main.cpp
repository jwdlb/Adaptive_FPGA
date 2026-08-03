#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

#include "app/config.hpp"
#include "app/opencl_devices.hpp"
#include "market/event.hpp"
#include "market/fixed_point.hpp"
#include "market/order_book.hpp"

namespace {
using namespace std;
// Creates a constant that is known at compile time and cannot be changed at runtime, std:string_view is a lightweight, read-only view of text. It does not own or copy the string. It simply refers to the existing characters.
constexpr string_view kVersion{"0.1.0"};

// Prints the contents of the order book. using BookSnapshot reference
void print_book(const market_engine::market::BookSnapshot& book) {
    const auto print_side = [](string_view name, const auto& levels) {
        std::cout << "  " << name << ':';
        for (const auto& level : levels) {
            if (level.quantity == 0U) break;
            std::cout << ' ' << level.price_ticks << '@' << level.quantity;
        }
        std::cout << '\n';
    };
    print_side("bids", book.bids);
    print_side("asks", book.asks);
}

// Writes to a CSV file containing the last 100 events leading up to a failure, so that the failure can be reproduced.
// Taakes in readonly list of all market events, and the position of teh event that failed.
void write_reproduction(const std::vector<market_engine::market::MarketEvent>& events, std::size_t failure_index) {
    const std::size_t first = failure_index > 100U ? failure_index - 100U : 0U;   // Caluclates the first of the last 100 events to record
    std::ofstream output("failure_repro.csv");  // Creates an output fiel stream to write the CSV file
    if (!output) return;   // If the file wasn't opended successfully, return early
    // Span is a lightweight view of contigous objects, used here as:
    //          std::span<const MarketEvent>(pointer, count)
    //    so events.data() + first is the pointer to the first event to write, and failure_index - first + 1U is the count of events to write.
    market_engine::market::write_csv(output, std::span<const market_engine::market::MarketEvent>(
        events.data() + static_cast<std::ptrdiff_t>(first), failure_index - first + 1U));   /// Calls project csv writing function to write the last 100 events to the file
}

// The program supports both CSV and binary event files. This function chooses the correct reader based on the file extension
[[nodiscard]] std::vector<market_engine::market::MarketEvent> read_events(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);    // Opens a file for reading in binary mode, which still works for csv on linux
    if (!input) throw std::runtime_error("cannot open event input: " + path.string());
    // Return right read handler based on file extension. .mkt and .bin are binary, everything else is csv.
    if (path.extension() == ".mkt" || path.extension() == ".bin") return market_engine::market::read_binary(input);
    return market_engine::market::read_csv(input);
}
}

int main(int argc, char* argv[]) {
    try {
        // This loop checks the command-line arguments for early informational command, start at 1 as 0 is teh program name (argv[0])
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);    // takes the current command-line argument and gives it the name argument, so we can use it in the following if statements
            if (argument == "--help" || argument == "-h") {
                std::cout << market_engine::app::usage(argv[0]);     // Give them the usage (in config.cpp) information and exit
                return 0;
            }
            if (argument == "--version") {
                std::cout << "market_engine_demo " << kVersion << '\n';   // Give them the version of the program and exit
                return 0;
            }
        }

        const market_engine::app::RuntimeOptions options = market_engine::app::parse_command_line(argc, argv);
        if (options.list_opencl_devices) {
            std::cout << market_engine::app::opencl_device_report();
            return 0;
        }
        
        // This section prints information about the program and checks whether an input file was supplied.
        std::cout << "Adaptive FPGA–GPU Market Signal Engine (reference replay)\n";
        std::cout << market_engine::app::format_config(options.config);
        std::cout << "Runtime mode: " << (options.reference_only ? "reference-only" : "C++ reference") << '\n';
        if (!options.input_path) {
            std::cout << "No input supplied; use --input PATH to replay CSV or MKT1 binary events.\n";
            return 0;
        }

        // This reads teh market events from teh input file
        std::vector<market_engine::market::MarketEvent> events = read_events(*options.input_path);

        // This section creates the main objects needed for replaying market events.
        market_engine::market::OrderBook book(options.config.allow_crossed_books);   // Creates an order book, config decides if crossed books are allowed (highest buy price (bid) that is higher than or equal to the lowest sell price (ask))
        market_engine::market::FeatureWindow window;    // Creates the 64-event rolling window used to store recent order-flow, trade-flow, and midpoint-movement data.
        // Creates a ModelParameters object to hold the model weights and thresholds, which are used to evaluate the market signal strategy. 
        // Creates the model’s parameters: - Eight model weights.  - Buy threshold.  - Sell threshold.  - Model version.  - Update count.     The {} value-initializes the object, so fields start at zero.
        market_engine::market::ModelParameters parameters{}; 
        parameters.buy_threshold = market_engine::market::fixed_point::from_double(options.config.buy_threshold);   // Converts the buy threshold set in config from a normal double, such as 0.2, into signed Q16.16 format.
        parameters.sell_threshold = market_engine::market::fixed_point::from_double(options.config.sell_threshold);    // Converts the sell threshold from a normal double, such as 0.2, into signed Q16.16 format.
        parameters.model_version = 1U;   // Sets the model version to 1, which is used to track changes to the model parameters over time.
        std::optional<std::int32_t> prior_midpoint;    // Stores the previous midpoint price, if one exists. It starts empty because there is no previous midpoint before the first event.
        market_engine::market::FeatureVector features{};   // Creates an initially empty feature vector. Its values start at zero and its valid flag starts as false.
        market_engine::market::Signal signal{};   // Creates an initially empty trading signal.  It starts with default values, such as:  - Score: 0  - Action: Hold  - Valid: false


        const auto count = std::min<std::size_t>(events.size(), options.event_limit.value_or(events.size()));   // number of events, or the max number of events allowed
        const auto started = std::chrono::steady_clock::now();   
        for (std::size_t index = 0; index < count; ++index) {       // for loop iterates through each event (index)
            // Applies the current market event to the order book, which updates the book’s state and returns an ApplyResult object that contains information about the result of the application, 
            // such as:  - Error: any error that occurred during application.  - Order flow delta: the change in order flow due to this event.  - Trade flow delta: the change in trade flow due to this event.
            const auto result = book.apply(events[index]);   
            if (result.error != market_engine::market::BookError::None) {   // if there is an error, do teh following:
                write_reproduction(events, index);  // write the last 100 events to a CSV file so that the failure can be reproduced
                std::cerr << "Replay error at event " << index << ": " << market_engine::market::to_string(result.error)
                          << "; wrote failure_repro.csv\n";    // Output the error to the console, so the user knows what went wrong and that a reproduction file was created
                return 1;
            }

            // Gets the current midpoint price.
            // The result is an std::optional<std::int32_t>:
            // - It contains a midpoint if both best bid and best ask exist.
            // - It is empty if either side of the book is empty.
            const auto midpoint = book.midpoint_ticks();

            // This calculates the absolute change from the previous midpoint.
            const auto midpoint_change = midpoint && prior_midpoint    // Checks that both optional values contain a value:
                ? std::llabs(static_cast<long long>(*midpoint) - *prior_midpoint) : 0LL;   // The * extracts the actual value from an std::optional, The cast widens the current midpoint to long long before subtraction., std::llabs Returns the absolute value, so both upward and downward movements become positive:

            window.push(result.order_flow_delta, result.trade_flow_delta, midpoint_change);  // adds the current event’s measurements to that window, The window keeps only the most recent 64 events. When a 65th event is added, the oldest event is removed automatically.
            if (midpoint) prior_midpoint = midpoint;    // If a valid current midpoint exists, it saves it as the previous midpoint for the next event.    If the midpoint is unavailable, prior_midpoint is left unchanged.
            features = market_engine::market::calculate_features(book, window);    // Calculates the current feature vector using:
            signal = market_engine::market::evaluate_strategy(events[index].timestamp_ns, index, features, parameters);      //  Evaluates the current trading signal and produces a signal such as BUY / SELL/ HOLD
        }

        const auto elapsed = std::chrono::steady_clock::now() - started;
        const auto seconds = std::chrono::duration<double>(elapsed).count();
        const auto checksum = market_engine::market::deterministic_checksum(book.snapshot(), features, signal, parameters);   // Numnerical fingerprint of the final state of the order book, feature vector, and signal. It is used to verify that the replay produced the expected result.
        std::cout << "Replay metrics:\n"
                  << "  processed events: " << count << '\n'
                  << "  errors: 0\n"
                  << "  events/s: " << std::fixed << std::setprecision(0) << (seconds > 0.0 ? count / seconds : 0.0) << '\n'
                  << "  final checksum: 0x" << std::hex << checksum << std::dec << '\n'
                  << "  final signal: " << market_engine::market::to_string(signal.action) << " (" << signal.score << ")\n"
                  << "Final book:\n";
        print_book(book.snapshot());
        return 0;
    } catch (const market_engine::app::ConfigError& error) {
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
