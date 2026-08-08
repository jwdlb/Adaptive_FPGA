#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>

#include "app/config.hpp"
#include "app/live_coordinator.hpp"
#include "app/opencl_devices.hpp"
#include "gpu/gpu_model.hpp"
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
        if (options.gpu_smoke_test) {
            const auto result = market_engine::gpu::run_gpu_smoke_test(
                options.gpu_index,
                options.gpu_name ? std::optional<std::string_view>(*options.gpu_name) : std::nullopt);
            if (result.status == market_engine::gpu::GpuSmokeTestStatus::skipped) {
                std::cout << "GPU smoke test skipped: " << result.message << '\n';
                return 0;
            }
            if (result.status == market_engine::gpu::GpuSmokeTestStatus::failed) {
                std::cerr << "GPU smoke test failed: " << result.message << '\n';
                return 1;
            }
            std::cout << "GPU smoke test passed: " << result.message << '\n';
            if (result.device) std::cout << market_engine::app::format_opencl_device(*result.device);
            return 0;
        }
        // GPU computation is introduced later in Phase 6. For now, a GPU selector
        // is an explicit setup check: prove which GPU future work will use.
        if (!options.gpu_feature_upload && (options.select_gpu || options.gpu_index || options.gpu_name)) {
            const auto selected = market_engine::app::select_opencl_gpu(
                options.gpu_index,
                options.gpu_name ? std::optional<std::string_view>(*options.gpu_name) : std::nullopt);
            std::cout << "Selected GPU:\n" << market_engine::app::format_opencl_device(selected);
            return 0;
        }

        // This section prints information about the program and checks whether an input file was supplied.
        std::cout << "Adaptive FPGA–GPU Market Signal Engine\n";
        std::cout << market_engine::app::format_config(options.config);
        const std::string_view mode = options.gpu_feature_upload ? "live RTL + GPU feature upload" : "live RTL";
        std::cout << "Runtime mode: " << mode << '\n';
        if (!options.input_path) {
            std::cout << "No input supplied; use --input PATH to provide CSV or MKT1 market events.\n";
            return 0;
        }

        // This reads the market events from the input file.
        const auto events = market_engine::io::read_events(*options.input_path);
        std::optional<market_engine::gpu::GpuModel> gpu_model;
        if (options.gpu_feature_upload) {
            gpu_model.emplace(
                options.gpu_index,
                options.gpu_name ? std::optional<std::string_view>(*options.gpu_name) : std::nullopt);
        }
        const market_engine::app::LiveCoordinator coordinator(options.config);
        const market_engine::app::LiveResult live = coordinator.run(
            events, options.event_limit, gpu_model ? &*gpu_model : nullptr);
        if (live.error) {
            std::cerr << "Live RTL error at event " << *live.failure_index << ": "
                      << market_engine::market::to_string(*live.error) << '\n';
            return 1;
        }
        const auto checksum = market_engine::market::deterministic_checksum(
            live.final_rtl.book, live.final_rtl.features, live.final_rtl.signal,
            live.active_parameters);
        std::cout << "Live metrics:\n"
                  << "  processed events: " << live.processed_events << '\n'
                  << "  errors: 0\n"
                  << "  events/s: " << std::fixed << std::setprecision(0)
                  << (live.elapsed_seconds > 0.0 ? live.processed_events / live.elapsed_seconds : 0.0) << '\n'
                  << "  RTL cycles: " << live.rtl_cycles << '\n'
                  << "  GPU feature batches submitted: " << live.gpu_feature_batches_submitted << '\n'
                  << "  GPU feature uploads completed: " << live.gpu_feature_uploads_completed << '\n'
                  << "  final checksum: 0x" << std::hex << checksum << std::dec << '\n'
                  << "  final signal: " << market_engine::market::to_string(live.final_rtl.signal.action)
                  << " (" << live.final_rtl.signal.score << ")\n"
                  << "Final book:\n";
        print_book(live.final_rtl.book);
        return 0;
    } catch (const market_engine::app::ConfigError& error) {
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
