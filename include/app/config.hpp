#pragma once  // Prevent this header from being included more than once.

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <stdexcept>
#include <string>

namespace market_engine::app {

// Values loaded from the JSON configuration file.
struct Config {
    std::uint32_t order_book_depth{10};
    std::uint32_t clock_period_ns{10};
    std::uint32_t feature_window_events{64};
    std::uint32_t feature_batch_size{1024};
    double learning_rate{0.001};
    double l2_regularisation{0.0001};
    std::uint32_t label_horizon_events{100};
    std::uint32_t weight_update_interval_batches{10};
    double buy_threshold{0.2};
    double sell_threshold{-0.2};
    std::uint16_t dashboard_port{8080};
    std::uint32_t dashboard_update_hz{10};
    bool enable_trace{false};
    bool enable_opencl_profiling{false};
    bool allow_crossed_books{false};
    std::uint64_t random_seed{42};
};

// Configuration plus options supplied on the command line at runtime.
struct RuntimeOptions {
    Config config{};
    std::filesystem::path config_path{"config/default.json"};
    std::optional<std::filesystem::path> input_path{};
    std::optional<std::uint64_t> event_limit{};
    bool reference_only{false};
    bool verilator_check{false};
    bool no_gpu{false};
    bool no_dashboard{false};
    bool benchmark{false};
    bool list_opencl_devices{false};
};

// Exception thrown when configuration loading, parsing, or validation fails.
class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Parse a JSON configuration from an input stream.
[[nodiscard]] Config parse_config(std::istream& input);
// Load and parse a JSON configuration file.
[[nodiscard]] Config load_config(const std::filesystem::path& path);
// Check that configuration values are within valid ranges.
void validate_config(const Config& config);
// Convert configuration values into a readable string.
[[nodiscard]] std::string format_config(const Config& config);
// Parse command-line arguments into runtime options.
[[nodiscard]] RuntimeOptions parse_command_line(int argc, char* argv[]);
// Return the command-line help text.
[[nodiscard]] std::string usage(std::string_view executable_name);

}  // namespace market_engine::app
