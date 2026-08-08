// Integration tests for Adaptive_FPGA's replay checker.
//
// The replay checker sends every market event through both the C++ reference
// model and the Verilated FPGA pipeline. These tests verify that both paths
// produce identical results for known CSV data and repeatable generated data.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "replay_coordinator.hpp"
#include "io/event_reader.hpp"
#include "market/order_book.hpp"

namespace {

using market_engine::market::BookSnapshot;
using market_engine::market::EventType;
using market_engine::market::MarketEvent;
using market_engine::market::OrderBook;
using market_engine::market::PriceLevel;
using market_engine::market::Side;

// Select a random occupied price level on one side of the book.
// Return nullptr when that side currently has no levels to modify or trade.
const PriceLevel* choose_level(const BookSnapshot& book, const Side side, std::mt19937& random) {
    const auto& levels = side == Side::Bid ? book.bids : book.asks;
    std::size_t size = 0;
    while (size < levels.size() && levels[size].quantity != 0U) ++size;
    if (size == 0U) return nullptr;
    return &levels[std::uniform_int_distribution<std::size_t>(0U, size - 1U)(random)];
}

// Generate one valid event based on the book's current state.
// The chosen operation is weighted toward adds, while updates, cancels, and
// trades only target price levels that already exist.
MarketEvent next_event(const BookSnapshot& book, const std::size_t index, std::mt19937& random) {
    const Side side = std::uniform_int_distribution<int>(0, 1)(random) == 0 ? Side::Bid : Side::Ask;
    const PriceLevel* existing = choose_level(book, side, random);
    const int choice = existing ? std::uniform_int_distribution<int>(0, 99)(random) : 0;
    const EventType type = !existing || choice < 45 ? EventType::Add
                         : choice < 60 ? EventType::Update
                         : choice < 80 ? EventType::Cancel : EventType::Trade;
    const std::int32_t price = type == EventType::Add
        ? std::uniform_int_distribution<std::int32_t>(side == Side::Bid ? 9991 : 10002,
                                                      side == Side::Bid ? 10000 : 10011)(random)
        : existing->price_ticks;
    const std::uint32_t quantity = type == EventType::Update
        ? std::uniform_int_distribution<std::uint32_t>(0U, 1000U)(random)
        : (type == EventType::Add ? std::uniform_int_distribution<std::uint32_t>(1U, 1000U)(random)
                                  : std::uniform_int_distribution<std::uint32_t>(1U, existing->quantity + 500U)(random));
    return {.timestamp_ns = 1'000U + index * 10U, .type = type, .side = side, .price_ticks = price, .quantity = quantity};
}

// Build a repeatable stream of valid events for RTL/reference comparison.
// A fixed seed makes every test run generate the exact same sequence.
std::vector<MarketEvent> deterministic_events(const std::size_t count) {
    OrderBook generator_book;
    std::mt19937 random{42U};
    std::vector<MarketEvent> events;
    events.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        // Keep a C++ book in step so each generated event remains valid.
        const MarketEvent event = next_event(generator_book.snapshot(), index, random);
        REQUIRE(generator_book.apply(event).error == market_engine::market::BookError::None);
        events.push_back(event);
    }
    return events;
}

// Run a full RTL-versus-reference replay and require a clean, complete match.
void require_matching_replay(const std::vector<MarketEvent>& events) {
    const market_engine::test_support::ReplayCoordinator coordinator(market_engine::app::Config{});
    const auto replay = coordinator.run_verilator_check(events, std::nullopt);
    REQUIRE_FALSE(replay.error.has_value());
    REQUIRE(replay.processed_events == events.size());
    REQUIRE(replay.rtl_cycles > 0U);
}

}  // namespace

TEST_CASE("Verilator check replay matches the directed CSV fixtures", "[replay][verilator][fixture]") {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    // These small hand-authored inputs cover specific known book behaviours.
    const std::filesystem::path fixture_directory = std::filesystem::path(MARKET_ENGINE_SOURCE_DIR) / "tests" / "fixtures";
    for (const std::string_view name : {"balanced_book.csv", "cancel_removes_level.csv"}) {
        INFO("fixture: " << name);
        require_matching_replay(market_engine::io::read_events(fixture_directory / name));
    }
#else
    SKIP("Verilator is unavailable in this build");
#endif
}

TEST_CASE("Verilator check replay matches a deterministic random stream", "[replay][verilator][random]") {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    // Exercise many valid event combinations using a reproducible random stream.
    require_matching_replay(deterministic_events(10'000U));
#else
    SKIP("Verilator is unavailable in this build");
#endif
}

TEST_CASE("Verilator check replay matches a one-million-event stream", "[replay][verilator][long]") {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    // A long regression run checks that the two implementations remain aligned
    // over sustained state changes, including rolling-window wraparound.
    require_matching_replay(deterministic_events(1'000'000U));
#else
    SKIP("Verilator is unavailable in this build");
#endif
}
