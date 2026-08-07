#include <exception>
#include <iomanip>
#include <iostream>
#include <string_view>

#include "app/config.hpp"
#include "app/opencl_devices.hpp"
#include "app/replay_coordinator.hpp"
#include "io/event_reader.hpp"
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
}  // namespace

int main(int argc, char* argv[]) {
    try {
        // This loop checks the command-line arguments for early informational command, start at 1 as 0 is teh program name (argv[0])
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);  // takes the current command-line argument and gives it the name argument, so we can use it in the following if statements
            if (argument == "--help" || argument == "-h") {
                std::cout << market_engine::app::usage(argv[0]);  // Give them the usage (in config.cpp) information and exit
                return 0;
            }
            if (argument == "--version") {
                std::cout << "market_engine_demo " << kVersion << '\n';  // Give them the version of the program and exit
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

        // This reads the market events from the input file.
        const auto events = market_engine::io::read_events(*options.input_path);
        // ReplayCoordinator owns the current reference loop and will later coordinate RTL and GPU modes.
        const market_engine::app::ReplayCoordinator coordinator(options.config);
        const market_engine::app::ReplayResult replay = coordinator.run_reference(events, options.event_limit);
        if (replay.error) {
            std::cerr << "Replay error at event " << *replay.failure_index << ": "
                      << market_engine::market::to_string(*replay.error)
                      << "; wrote failure_repro.csv\n";
            return 1;
        }

        // Numerical fingerprint of the final order book, feature vector, signal, and model parameters.
        const auto checksum = market_engine::market::deterministic_checksum(
            replay.final_book, replay.final_features, replay.final_signal, replay.final_parameters);
        std::cout << "Replay metrics:\n"
                  << "  processed events: " << replay.processed_events << '\n'
                  << "  errors: 0\n"
                  << "  events/s: " << std::fixed << std::setprecision(0)
                  << (replay.elapsed_seconds > 0.0 ? replay.processed_events / replay.elapsed_seconds : 0.0) << '\n'
                  << "  final checksum: 0x" << std::hex << checksum << std::dec << '\n'
                  << "  final signal: " << market_engine::market::to_string(replay.final_signal.action)
                  << " (" << replay.final_signal.score << ")\n"
                  << "Final book:\n";
        print_book(replay.final_book);
        return 0;
    } catch (const market_engine::app::ConfigError& error) {
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
