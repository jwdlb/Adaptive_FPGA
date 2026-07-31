#include "app/config.hpp"

#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

#include <nlohmann/json.hpp>

namespace market_engine::app {
namespace {

template <typename T>
void read_if_present(const nlohmann::json& json, const char* name, T& value) {
    if (json.contains(name)) {
        value = json.at(name).get<T>();
    }
}

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

[[nodiscard]] const char* require_value(int& index, int argc, char* argv[], std::string_view option) {
    if (++index >= argc) {
        throw ConfigError("missing value for " + std::string(option));
    }
    return argv[index];
}

}  // namespace

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

Config load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw ConfigError("cannot open configuration file: " + path.string());
    }
    return parse_config(input);
}

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

RuntimeOptions parse_command_line(int argc, char* argv[]) {
    RuntimeOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--config") {
            options.config_path = require_value(index, argc, argv, argument);
        }
    }
    options.config = load_config(options.config_path);

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
            const auto value = parse_unsigned_option(argument, require_value(index, argc, argv, argument));
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw ConfigError("--batch-size is too large");
            }
            options.config.feature_batch_size = static_cast<std::uint32_t>(value);
        } else if (argument == "--reference-only") {
            options.reference_only = true;
        } else if (argument == "--no-gpu") {
            options.no_gpu = true;
        } else if (argument == "--no-dashboard") {
            options.no_dashboard = true;
        } else if (argument == "--trace") {
            options.config.enable_trace = true;
        } else if (argument == "--benchmark") {
            options.benchmark = true;
        } else if (argument == "--list-opencl-devices") {
            options.list_opencl_devices = true;
        } else if (argument == "--help" || argument == "-h" || argument == "--version") {
            continue;
        } else {
            throw ConfigError("unknown option: " + std::string(argument));
        }
    }
    if (options.event_limit.has_value() && *options.event_limit == 0U) {
        throw ConfigError("--events must be positive");
    }
    validate_config(options.config);
    return options;
}

std::string usage(std::string_view executable_name) {
    return "Usage: " + std::string(executable_name) + " [options]\n"
           "  --config PATH              Configuration JSON (default: config/default.json)\n"
           "  --input PATH               Market-event input (future phase)\n"
           "  --events N                 Replay limit (future phase)\n"
           "  --seed N                   Override random seed\n"
           "  --batch-size N             Override GPU batch size\n"
           "  --reference-only           Disable RTL/GPU runtime paths\n"
           "  --no-gpu                   Disable GPU runtime path\n"
           "  --no-dashboard             Disable dashboard runtime path\n"
           "  --trace                    Request RTL tracing (future phase)\n"
           "  --benchmark                Enable benchmark mode (future phase)\n"
           "  --list-opencl-devices      Print detected OpenCL devices\n"
           "  --version                  Print program version\n"
           "  --help, -h                 Print this help\n";
}

}  // namespace market_engine::app
