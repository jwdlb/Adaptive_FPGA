// Integration tests for the normal RTL application path. These tests do not
// compare against C++; they prove LiveCoordinator can process an event stream
// using only the simulated RTL pipeline.
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "app/live_coordinator.hpp"
#include "io/event_reader.hpp"

TEST_CASE("live coordinator processes RTL events without a reference comparison", "[live][verilator]") {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    const std::filesystem::path input = std::filesystem::path(MARKET_ENGINE_SOURCE_DIR) /
        "tests" / "fixtures" / "balanced_book.csv";
    const auto events = market_engine::io::read_events(input);

    const market_engine::app::LiveCoordinator coordinator(market_engine::app::Config{});
    const auto result = coordinator.run(events, std::nullopt);

    REQUIRE_FALSE(result.error.has_value());
    REQUIRE(result.processed_events == events.size());
    REQUIRE(result.rtl_cycles > 0U);
    REQUIRE(result.final_rtl.signal.model_version == 1U);
#else
    SKIP("Verilator is unavailable in this build");
#endif
}
