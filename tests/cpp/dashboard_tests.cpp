#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <type_traits>

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "app/dashboard_server.hpp"
#include "app/live_coordinator.hpp"
#include "app/market_data_generator.hpp"
#include "io/event_reader.hpp"
#include "market/order_book.hpp"

namespace {
namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

std::filesystem::path generated_fixture() {
    const auto path = std::filesystem::temp_directory_path() / "adaptive-fpga-dashboard-test.csv";
    market_engine::app::generate_market_csv(path, 91U, 4096U);
    return path;
}

std::uint64_t checksum(const market_engine::app::LiveResult& result) {
    return market_engine::market::deterministic_checksum(result.final_rtl.book, result.final_rtl.features,
                                                          result.final_rtl.signal, result.active_parameters);
}
}  // namespace

TEST_CASE("dashboard snapshot serializes the complete versioned contract") {
    static_assert(std::is_copy_constructible_v<market_engine::app::DashboardSnapshot>);
    market_engine::app::DashboardSnapshot snapshot;
    snapshot.sequence = 7; snapshot.connection_state = "connected"; snapshot.state = "running";
    snapshot.activity_state = "generating"; snapshot.activity_message = "Generating test.csv";
    snapshot.activity_completed = 50; snapshot.activity_total = 100;
    snapshot.processed_events = 99; snapshot.events_per_second = 1234.5;
    snapshot.features.values = {1,2,3,4,5,6,7,8}; snapshot.features.valid = true;
    snapshot.model.weights = {8,7,6,5,4,3,2,1}; snapshot.model.model_version = 4;
    snapshot.signal.action = market_engine::market::Action::Buy;
    snapshot.recent_events.push_back({1000, market_engine::market::EventType::Add,
                                      market_engine::market::Side::Bid, 9999, 10});
    const auto json = nlohmann::json::parse(market_engine::app::dashboard_snapshot_json(snapshot));
    REQUIRE(json.at("schemaVersion") == 1);
    REQUIRE(json.at("sequence") == 7);
    REQUIRE(json.at("activity").at("state") == "generating");
    REQUIRE(json.at("activity").at("completed") == 50);
    REQUIRE(json.at("featuresQ16").size() == 8);
    REQUIRE(json.at("model").at("weightsQ16").size() == 8);
    REQUIRE(json.at("signal").at("action") == "Buy");
    REQUIRE(json.at("recentEvents").at(0).at("priceTicks") == 9999);
    REQUIRE(json.contains("performance"));
}

TEST_CASE("dashboard CSV generation is deterministic and replay-readable") {
    const auto first = std::filesystem::temp_directory_path() / "adaptive-fpga-generated-a.csv";
    const auto second = std::filesystem::temp_directory_path() / "adaptive-fpga-generated-b.csv";
    market_engine::app::generate_market_csv(first, 123U, 512U);
    market_engine::app::generate_market_csv(second, 123U, 512U);
    const auto first_events = market_engine::io::read_events(first);
    const auto second_events = market_engine::io::read_events(second);
    REQUIRE(first_events.size() == 512U);
    REQUIRE(first_events == second_events);
    std::filesystem::remove(first); std::filesystem::remove(second);
}

TEST_CASE("dashboard handles WebSocket lifecycle and malformed HTTP") {
    auto store = std::make_shared<market_engine::app::DashboardSnapshotStore>();
    market_engine::app::DashboardServer server({"127.0.0.1", 0, 20}, store);
    server.start();

    asio::io_context ioc;
    websocket::stream<tcp::socket> ws(ioc);
    ws.next_layer().connect({asio::ip::make_address("127.0.0.1"), server.local_port()});
    ws.handshake("127.0.0.1", "/ws");
    beast::flat_buffer frame;
    ws.read(frame);
    REQUIRE(nlohmann::json::parse(beast::buffers_to_string(frame.data())).at("schemaVersion") == 1);
    REQUIRE(server.client_count() == 1U);
    ws.close(websocket::close_code::normal);
    for (int attempt = 0; attempt < 50 && server.client_count() != 0U; ++attempt) std::this_thread::sleep_for(2ms);
    REQUIRE(server.client_count() == 0U);

    tcp::socket malformed(ioc);
    malformed.connect({asio::ip::make_address("127.0.0.1"), server.local_port()});
    asio::write(malformed, asio::buffer(std::string("NOT HTTP\r\n\r\n")));
    std::array<char, 512> reply{};
    const std::size_t received = malformed.read_some(asio::buffer(reply));
    REQUIRE(std::string_view(reply.data(), received).find("400 Bad Request") != std::string_view::npos);
    server.stop();
}

#if MARKET_ENGINE_VERILATOR_AVAILABLE
TEST_CASE("a slow dashboard client cannot change replay output") {
    const auto path = generated_fixture();
    const auto events = market_engine::io::read_events(path);
    market_engine::app::Config config; config.dashboard_update_hz = 100;
    market_engine::app::LiveCoordinator coordinator(config);

    const auto off_started = std::chrono::steady_clock::now();
    const auto without = coordinator.run(events, std::nullopt, nullptr);
    const double off_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - off_started).count();

    auto store = std::make_shared<market_engine::app::DashboardSnapshotStore>();
    market_engine::app::DashboardServer server({"127.0.0.1", 0, 100}, store);
    server.start();
    asio::io_context ioc;
    websocket::stream<tcp::socket> slow(ioc);
    slow.next_layer().connect({asio::ip::make_address("127.0.0.1"), server.local_port()});
    slow.handshake("127.0.0.1", "/ws");
    // Deliberately never read a frame while replay executes.
    const auto on_started = std::chrono::steady_clock::now();
    const auto with = coordinator.run(events, std::nullopt, nullptr, std::nullopt, {}, store, path.string());
    const double on_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - on_started).count();
    CAPTURE(off_ms, on_ms, on_ms - off_ms, on_ms / off_ms);
    std::cout << "Dashboard benchmark (4096 events): off=" << off_ms << " ms, on=" << on_ms
              << " ms, overhead=" << (on_ms - off_ms) << " ms (" << (on_ms / off_ms) << "x)\n";
    REQUIRE(checksum(without) == checksum(with));
    REQUIRE(with.processed_events == without.processed_events);
    beast::error_code ignored; slow.close(websocket::close_code::normal, ignored);
    server.stop();
    std::filesystem::remove(path);
}
#endif
