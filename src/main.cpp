#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <string_view>

#include <nlohmann/json.hpp>

#include "app/config.hpp"
#include "app/dashboard_server.hpp"
#include "app/live_coordinator.hpp"
#include "app/market_data_generator.hpp"
#include "app/model_store.hpp"
#include "app/opencl_devices.hpp"
#include "gpu/gpu_model.hpp"
#include "io/event_reader.hpp"
#include "market/order_book.hpp"

namespace {
using namespace std;
// Creates a constant that is known at compile time and cannot be changed at runtime, std:string_view is a lightweight, read-only view of text. It does not own or copy the string. It simply refers to the existing characters.
constexpr string_view kVersion{"0.1.0"};
std::atomic_bool stop_dashboard{false};
void handle_stop_signal(int) { stop_dashboard.store(true, std::memory_order_relaxed); }

class DashboardCommands {
public:
    DashboardCommands(market_engine::app::RuntimeOptions options,
                      std::shared_ptr<market_engine::app::DashboardSnapshotStore> snapshots)
        : options_(std::move(options)), snapshots_(std::move(snapshots)), worker_([this] { work(); }) {}
    ~DashboardCommands() {
        { std::lock_guard lock(mutex_); stopping_ = true; }
        ready_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    std::string submit(std::string_view body) {
        const auto command = nlohmann::json::parse(body);
        if (!command.is_object()) throw std::invalid_argument("control command must be a JSON object");
        const std::string action = command.value("action", "");
        if (action != "generate" && action != "run") throw std::invalid_argument("action must be generate or run");
        if ((action == "generate" && command.value("output", "").empty()) ||
            (action == "run" && command.value("input", "").empty())) throw std::invalid_argument("a file path is required");
        if (command.contains("events") && command.at("events").get<std::uint64_t>() == 0U) {
            throw std::invalid_argument("events must be positive");
        }
        std::lock_guard lock(mutex_);
        if (commands_.size() >= 8U) throw std::runtime_error("dashboard command queue is full");
        commands_.push_back(command); ready_.notify_one();
        return nlohmann::json{{"accepted", true}, {"action", action}, {"queued", commands_.size()}}.dump();
    }
private:
    void publish_state(std::string state) {
        auto snapshot = *snapshots_->latest(); snapshot.state = std::move(state);
        snapshot.published_at_unix_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
        snapshots_->publish(std::move(snapshot));
    }
    void work() {
        while (true) {
            nlohmann::json command;
            {
                std::unique_lock lock(mutex_); ready_.wait(lock, [this] { return stopping_ || !commands_.empty(); });
                if (stopping_) return;
                command = std::move(commands_.front()); commands_.pop_front();
            }
            try {
                if (command.at("action") == "generate") {
                    publish_state("generating");
                    const auto count = command.value<std::uint64_t>("events", 100000U);
                    market_engine::app::generate_market_csv(command.at("output").get<std::string>(),
                                                            command.value<std::uint64_t>("seed", options_.config.random_seed), count);
                    publish_state("idle");
                } else {
                    const std::filesystem::path input = command.at("input").get<std::string>();
                    const auto events = market_engine::io::read_events(input);
                    std::optional<std::uint64_t> limit;
                    if (command.contains("events")) limit = command.at("events").get<std::uint64_t>();
                    std::optional<market_engine::gpu::GpuModel> gpu;
                    if (command.value("gpu", false)) {
                        gpu.emplace(options_.gpu_index, options_.gpu_name ? std::optional<std::string_view>(*options_.gpu_name) : std::nullopt);
                    }
                    const market_engine::app::LiveCoordinator coordinator(options_.config);
                    static_cast<void>(coordinator.run(events, limit, gpu ? &*gpu : nullptr, std::nullopt, {}, snapshots_, input.string()));
                }
            } catch (const std::exception& error) {
                std::cerr << "Dashboard command failed: " << error.what() << '\n';
                publish_state("failed");
            }
        }
    }
    market_engine::app::RuntimeOptions options_;
    std::shared_ptr<market_engine::app::DashboardSnapshotStore> snapshots_;
    std::mutex mutex_; std::condition_variable ready_; std::deque<nlohmann::json> commands_;
    bool stopping_{}; std::thread worker_;
};

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
        // Without the live GPU-worker option, a selector is an explicit setup
        // check: prove which GPU the normal streaming path will use.
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
        const std::string_view mode = options.gpu_feature_upload ? "live RTL + GPU worker" : "live RTL";
        std::cout << "Runtime mode: " << mode << '\n';
        std::shared_ptr<market_engine::app::DashboardSnapshotStore> dashboard_snapshots;
        if (!options.no_dashboard) dashboard_snapshots = std::make_shared<market_engine::app::DashboardSnapshotStore>();
        if (!options.input_path) {
            if (options.no_dashboard) {
                std::cout << "No input supplied; use --input PATH to provide CSV or MKT1 market events.\n";
                return 0;
            }
            DashboardCommands commands(options, dashboard_snapshots);
            auto idle_snapshot = *dashboard_snapshots->latest();
            idle_snapshot.connection_state = "listening";
            dashboard_snapshots->publish(std::move(idle_snapshot));
            market_engine::app::DashboardServer dashboard(
                {options.config.dashboard_bind_address, options.config.dashboard_port, options.config.dashboard_update_hz},
                dashboard_snapshots, [&](std::string_view body) { return commands.submit(body); });
            dashboard.start();
            std::cout << "Dashboard control mode: http://" << options.config.dashboard_bind_address << ':'
                      << dashboard.local_port() << "/ (Ctrl-C to stop)\n";
            std::signal(SIGINT, handle_stop_signal);
            std::signal(SIGTERM, handle_stop_signal);
            while (!stop_dashboard.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            dashboard.stop();
            return 0;
        }

        std::optional<market_engine::app::DashboardServer> dashboard;
        if (dashboard_snapshots) {
            dashboard.emplace(
                market_engine::app::DashboardServerOptions{options.config.dashboard_bind_address,
                                                           options.config.dashboard_port,
                                                           options.config.dashboard_update_hz},
                dashboard_snapshots);
            dashboard->start();
            std::cout << "Dashboard: http://" << options.config.dashboard_bind_address << ':' << dashboard->local_port() << "/\n";
        }

        // This reads the market events from the input file.
        const auto events = market_engine::io::read_events(*options.input_path);
        std::optional<market_engine::market::ModelParameters> initial_model;
        if (options.model_in && !options.reset_model) initial_model = market_engine::app::load_model_file(*options.model_in);
        std::optional<market_engine::gpu::GpuModel> gpu_model;
        if (options.gpu_feature_upload) {
            gpu_model.emplace(
                options.gpu_index,
                options.gpu_name ? std::optional<std::string_view>(*options.gpu_name) : std::nullopt);
            if (initial_model) gpu_model->set_training_model(*initial_model);
        }
        const market_engine::app::LiveCoordinator coordinator(options.config);
        const market_engine::app::LiveResult live = coordinator.run(
            events, options.event_limit, gpu_model ? &*gpu_model : nullptr, initial_model,
            options.model_autosave ? [path = *options.model_autosave](const auto& model) {
                market_engine::app::save_model_file_atomically(path, model);
            } : std::function<void(const market_engine::market::ModelParameters&)>{},
            dashboard_snapshots, options.input_path->string());
        if (live.error) {
            std::cerr << "Live RTL error at event " << *live.failure_index << ": "
                      << market_engine::market::to_string(*live.error) << '\n';
            return 1;
        }
        if (options.model_out) market_engine::app::save_model_file_atomically(*options.model_out, live.active_parameters);
        const auto checksum = market_engine::market::deterministic_checksum(
            live.final_rtl.book, live.final_rtl.features, live.final_rtl.signal,
            live.active_parameters);
        std::cout << "Live metrics:\n"
                  << "  processed events: " << live.processed_events << '\n'
                  << "  errors: 0\n"
                  << "  events/s: " << std::fixed << std::setprecision(0)
                  << (live.elapsed_seconds > 0.0 ? live.processed_events / live.elapsed_seconds : 0.0) << '\n'
                  << "  RTL cycles: " << live.rtl_cycles << '\n'
                  << "  RTL stream results published: " << live.rtl_stream_results_published << '\n'
                  << "  GPU RTL results consumed: " << live.gpu_rtl_results_consumed << '\n'
                  << "  GPU valid feature rows copied: " << live.gpu_valid_feature_rows_copied << '\n'
                  << "  GPU batches submitted: " << live.gpu_batches_submitted << '\n'
                  << "  GPU model updates published: " << live.gpu_model_updates_published << '\n'
                  << "  GPU batch squared error (Q16.16): " << live.gpu_squared_error_sum_q16 << '\n'
                  << "  GPU batch accuracy: " << (live.gpu_training_rows == 0U ? 0.0 :
                      static_cast<double>(live.gpu_correct_predictions) / live.gpu_training_rows) << '\n'
                  << "  GPU upload/kernel/readback ms: " << live.gpu_upload_ms << '/' << live.gpu_kernel_ms
                  << '/' << live.gpu_readback_ms << '\n'
                  << "  GPU update latency ms: " << live.gpu_update_latency_ms << '\n'
                  << "  RTL model updates applied: " << live.rtl_model_updates_applied << '\n'
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
