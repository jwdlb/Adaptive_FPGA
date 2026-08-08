#include "app/config.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>

namespace market_engine::app {
namespace {

//  This is a reusable helper function for reading a configuration value from JSON.
// Generic function so can be used for any type T. It takes in a JSON object, the name of the value to read, and a reference to the variable to store the value in. If the value is present in the JSON object, it reads it and assigns it to the variable.
template <typename T>
void read_if_present(const nlohmann::json& json, const char* name, T& value) {
    if (json.contains(name)) {
        value = json.at(name).get<T>();
    }
}

// Declares a helper function that converts a command-line value into an unsigned 64-bit integer.
[[nodiscard]] std::uint64_t parse_unsigned_option(std::string_view option, std::string_view value) {
    std::size_t consumed = 0;
    std::uint64_t result = 0;
    try {
        result = std::stoull(std::string(value), &consumed, 10);
    } catch (const std::exception&) {
        throw ConfigError("invalid value for " + std::string(option) + ": " + std::string(value));
    }
    if (consumed != value.size()) {
        throw ConfigError("invalid value for " + std::string(option) + ": " + std::string(value));
    }
    return result;
}

//   This helper obtains the value that comes immediately after a command-line option.
[[nodiscard]] const char* require_value(int& index, int argc, char* argv[], std::string_view option) {
    if (++index >= argc) {
        throw ConfigError("missing value for " + std::string(option));
    }
    return argv[index];
}

}  // namespace

// This function reads JSON configuration data, converts it into a Config object, validates it, and returns it.
Config parse_config(std::istream& input) {
    nlohmann::json json;
    try {
        input >> json;
    } catch (const nlohmann::json::exception& error) {
        throw ConfigError("invalid JSON configuration: " + std::string(error.what()));
    }

    if (!json.is_object()) {
        throw ConfigError("configuration root must be an object");
    }

    Config config;
    try {
        read_if_present(json, "orderBookDepth", config.order_book_depth);
        read_if_present(json, "clockPeriodNs", config.clock_period_ns);
        read_if_present(json, "featureWindowEvents", config.feature_window_events);
        read_if_present(json, "featureBatchSize", config.feature_batch_size);
        read_if_present(json, "learningRate", config.learning_rate);
        read_if_present(json, "l2Regularisation", config.l2_regularisation);
        read_if_present(json, "labelHorizonEvents", config.label_horizon_events);
        read_if_present(json, "weightUpdateIntervalBatches", config.weight_update_interval_batches);
        read_if_present(json, "buyThreshold", config.buy_threshold);
        read_if_present(json, "sellThreshold", config.sell_threshold);
        read_if_present(json, "dashboardPort", config.dashboard_port);
        read_if_present(json, "dashboardUpdateHz", config.dashboard_update_hz);
        read_if_present(json, "enableTrace", config.enable_trace);
        read_if_present(json, "enableOpenCLProfiling", config.enable_opencl_profiling);
        read_if_present(json, "allowCrossedBooks", config.allow_crossed_books);
        read_if_present(json, "randomSeed", config.random_seed);
    } catch (const nlohmann::json::exception& error) {
        throw ConfigError("invalid configuration value: " + std::string(error.what()));
    }

    validate_config(config);
    return config;
}

// This function loads a JSON configuration file from the specified path and returns a Config object. If the file cannot be opened, it throws a ConfigError.
Config load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw ConfigError("cannot open configuration file: " + path.string());
    }
    return parse_config(input);
}

// This function checks that the configuration contains valid values.
void validate_config(const Config& config) {
    if (config.order_book_depth != 10U) {
        throw ConfigError("orderBookDepth must be 10 in version 1");
    }
    if (config.clock_period_ns == 0U || config.feature_window_events == 0U || config.feature_batch_size == 0U ||
        config.label_horizon_events == 0U || config.weight_update_interval_batches == 0U ||
        config.dashboard_update_hz == 0U) {
        throw ConfigError("clock, windows, batch size, horizons, update interval, and dashboard rate must be positive");
    }
    if (!std::isfinite(config.learning_rate) || config.learning_rate <= 0.0) {
        throw ConfigError("learningRate must be finite and positive");
    }
    if (!std::isfinite(config.l2_regularisation) || config.l2_regularisation < 0.0) {
        throw ConfigError("l2Regularisation must be finite and non-negative");
    }
    if (!std::isfinite(config.buy_threshold) || !std::isfinite(config.sell_threshold) ||
        config.buy_threshold <= config.sell_threshold) {
        throw ConfigError("buyThreshold must be finite and greater than sellThreshold");
    }
}

std::string format_config(const Config& config) {
    std::ostringstream output;
    output << "Configuration:\n"
           << "  orderBookDepth: " << config.order_book_depth << "\n"
           << "  clockPeriodNs: " << config.clock_period_ns << "\n"
           << "  featureWindowEvents: " << config.feature_window_events << "\n"
           << "  featureBatchSize: " << config.feature_batch_size << "\n"
           << "  learningRate: " << config.learning_rate << "\n"
           << "  l2Regularisation: " << config.l2_regularisation << "\n"
           << "  labelHorizonEvents: " << config.label_horizon_events << "\n"
           << "  weightUpdateIntervalBatches: " << config.weight_update_interval_batches << "\n"
           << "  buyThreshold: " << config.buy_threshold << "\n"
           << "  sellThreshold: " << config.sell_threshold << "\n"
           << "  dashboardPort: " << config.dashboard_port << "\n"
           << "  dashboardUpdateHz: " << config.dashboard_update_hz << "\n"
           << "  enableTrace: " << std::boolalpha << config.enable_trace << "\n"
           << "  enableOpenCLProfiling: " << config.enable_opencl_profiling << "\n"
           << "  allowCrossedBooks: " << config.allow_crossed_books << "\n"
           << "  randomSeed: " << config.random_seed << '\n';
    return output.str();
}

// A function that reads the program’s command-line arguments and converts them into a RuntimeOptions object
RuntimeOptions parse_command_line(int argc, char* argv[]) {
    RuntimeOptions options;     // Creates one RuntimeOptions object named options

    // Basically just getting teh config path from the command line, if it is not there, it will use the default path in the RuntimeOptions struct
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--config") {
            options.config_path = require_value(index, argc, argv, argument);   // If the argument is --config, it calls require_value to get the next argument as the config path
        }
    }
    options.config = load_config(options.config_path);

    // Iterates through teh command line argumnets and sets the corresponding fields in the RuntimeOptions object based on the options provided. If an unknown option is encountered, it throws a ConfigError.
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--config") {
            ++index;
        } else if (argument == "--input") {
            options.input_path = require_value(index, argc, argv, argument);
        } else if (argument == "--events") {
            options.event_limit = parse_unsigned_option(argument, require_value(index, argc, argv, argument));
        } else if (argument == "--seed") {
            options.config.random_seed = parse_unsigned_option(argument, require_value(index, argc, argv, argument));
        } else if (argument == "--batch-size") {
            const uint64_t value = parse_unsigned_option(argument, require_value(index, argc, argv, argument));
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw ConfigError("--batch-size is too large");
            }
            options.config.feature_batch_size = static_cast<std::uint32_t>(value);
        } else if (argument == "--reference-only") {
            options.reference_only = true;
        } else if (argument == "--verilator-check") {
            options.verilator_check = true;
        } else if (argument == "--no-gpu") {
            options.no_gpu = true;
        } else if (argument == "--gpu-index") {
            const std::uint64_t value = parse_unsigned_option(argument, require_value(index, argc, argv, argument));
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw ConfigError("--gpu-index is too large");
            }
            options.gpu_index = static_cast<std::uint32_t>(value);
        } else if (argument == "--gpu-name") {
            options.gpu_name = require_value(index, argc, argv, argument);
            if (options.gpu_name->empty()) {
                throw ConfigError("--gpu-name must not be empty");
            }
        } else if (argument == "--no-dashboard") {
            options.no_dashboard = true;
        } else if (argument == "--trace") {
            options.config.enable_trace = true;
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else if (argument == "--list-opencl-devices") {
            options.list_opencl_devices = true;
        } else if (argument == "--select-gpu") {
            options.select_gpu = true;
        } else if (argument == "--help" || argument == "-h" || argument == "--version") {
            continue;
        } else {
            throw ConfigError("unknown option: " + std::string(argument));
        }
    }
    if (options.event_limit.has_value() && *options.event_limit == 0U) {
        throw ConfigError("--events must be positive");
    }
    if (options.reference_only && options.verilator_check) {
        throw ConfigError("--reference-only and --verilator-check cannot be used together");
    }
    if (options.gpu_index && options.gpu_name) {
        throw ConfigError("--gpu-index and --gpu-name cannot be used together");
    }
    if (options.no_gpu && (options.select_gpu || options.gpu_index || options.gpu_name)) {
        throw ConfigError("--no-gpu cannot be used with --select-gpu, --gpu-index, or --gpu-name");
    }
    validate_config(options.config);
    return options;
}

// This function returns a string containing the usage information for the program, including the command-line options and their descriptions.
std::string usage(std::string_view executable_name) {
    return "Usage: " + std::string(executable_name) + " [options]\n"
           "  --config PATH              Configuration JSON (default: config/default.json)\n"
           "  --input PATH               CSV or MKT1 binary market-event input\n"
           "  --events N                 Replay only the first N events\n"
           "  --seed N                   Override random seed\n"
           "  --batch-size N             Override GPU batch size\n"
           "  --reference-only           Disable RTL/GPU runtime paths\n"
           "  --verilator-check          Compare every replay event against the Verilated RTL pipeline\n"
           "  --no-gpu                   Disable GPU runtime path\n"
           "  --gpu-index N              Select GPU N (or inspect its availability)\n"
           "  --gpu-name TEXT            Select a GPU by part of its name\n"
           "  --no-dashboard             Disable dashboard runtime path\n"
           "  --trace                    Request RTL tracing (future phase)\n"
           "  --benchmark                Reserved for a future benchmark mode\n"
           "  --list-opencl-devices      Print detected OpenCL devices\n"
           "  --select-gpu               Select the first available GPU, or the requested GPU\n"
           "  --version                  Print program version\n"
           "  --help, -h                 Print this help\n";
}

}  // namespace market_engine::app
