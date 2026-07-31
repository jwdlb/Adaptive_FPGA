#include <exception>
#include <iostream>
#include <string_view>

#include "app/config.hpp"
#include "app/opencl_devices.hpp"

namespace {
constexpr std::string_view kVersion{"0.1.0"};
}

int main(int argc, char* argv[]) {
    try {
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--help" || argument == "-h") {
                std::cout << market_engine::app::usage(argv[0]);
                return 0;
            }
            if (argument == "--version") {
                std::cout << "market_engine_demo " << kVersion << '\n';
                return 0;
            }
        }

        const auto options = market_engine::app::parse_command_line(argc, argv);
        if (options.list_opencl_devices) {
            std::cout << market_engine::app::opencl_device_report();
            return 0;
        }

        std::cout << "Adaptive FPGA–GPU Market Signal Engine (Phase 0)\n";
        std::cout << market_engine::app::format_config(options.config);
        std::cout << "Runtime mode: " << (options.reference_only ? "reference-only" : "foundation only") << '\n';
        std::cout << "The market replay, RTL model, GPU learner, and dashboard are not implemented yet.\n";
        return 0;
    } catch (const market_engine::app::ConfigError& error) {
        std::cerr << "Configuration error: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
