#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "market/event.hpp"
#include "market/fixed_point.hpp"

namespace {
using market_engine::market::EventCodecError;
using market_engine::market::EventType;
using market_engine::market::EventValidationError;
using market_engine::market::MarketEvent;
using market_engine::market::Side;

[[nodiscard]] std::vector<MarketEvent> sample_events() {
    return {
        {1000U, EventType::Add, Side::Bid, 10001, 500U},
        {1010U, EventType::Trade, Side::Ask, 10002, 100U},
        {1020U, EventType::Update, Side::Bid, 10001, 0U},
    };
}
}  // namespace

TEST_CASE("market events validate their canonical rules") {
    MarketEvent event{1U, EventType::Add, Side::Bid, 1, 1U};
    REQUIRE(market_engine::market::validate_event(event) == EventValidationError::None);
    event.price_ticks = 0;
    REQUIRE(market_engine::market::validate_event(event) == EventValidationError::NonPositivePrice);
    event = {1U, EventType::Add, Side::Bid, 1, 0U};
    REQUIRE(market_engine::market::validate_event(event) == EventValidationError::ZeroQuantity);
    event = {1U, EventType::Update, Side::Bid, 1, 0U};
    REQUIRE(market_engine::market::validate_event(event) == EventValidationError::None);
    event.side = static_cast<Side>(9U);
    REQUIRE(market_engine::market::validate_event(event) == EventValidationError::InvalidSide);
    event = {1U, static_cast<EventType>(9U), Side::Bid, 1, 1U};
    REQUIRE(market_engine::market::validate_event(event) == EventValidationError::InvalidEventType);
}

TEST_CASE("CSV codec round trips and rejects bad records") {
    const auto events = sample_events();
    std::ostringstream output;
    market_engine::market::write_csv(output, events);
    std::istringstream round_trip(output.str());
    REQUIRE(market_engine::market::read_csv(round_trip) == events);

    std::istringstream malformed(
        "timestamp_ns,event_type,side,price_ticks,quantity\n"
        "1000,Add,Bid,10001,0\n");
    REQUIRE_THROWS_AS(market_engine::market::read_csv(malformed), EventCodecError);
    std::istringstream missing_header("");
    REQUIRE_THROWS_AS(market_engine::market::read_csv(missing_header), EventCodecError);
}

TEST_CASE("binary codec is versioned and round trips") {
    const auto events = sample_events();
    std::ostringstream output(std::ios::binary);
    market_engine::market::write_binary(output, events);
    const std::string bytes = output.str();
    REQUIRE(bytes.substr(0U, 4U) == "MKT1");
    REQUIRE(static_cast<unsigned char>(bytes[4]) == 1U);
    REQUIRE(bytes.size() == 16U + 20U * events.size());
    std::istringstream round_trip(bytes, std::ios::binary);
    REQUIRE(market_engine::market::read_binary(round_trip) == events);

    std::istringstream truncated("MKT1", std::ios::binary);
    REQUIRE_THROWS_AS(market_engine::market::read_binary(truncated), EventCodecError);
    std::string bad_version = bytes;
    bad_version[4] = static_cast<char>(2);
    std::istringstream unsupported(bad_version, std::ios::binary);
    REQUIRE_THROWS_AS(market_engine::market::read_binary(unsupported), EventCodecError);
    std::string non_zero_reserved = bytes;
    non_zero_reserved[26] = static_cast<char>(1);  // First record's little-endian reserved field.
    std::istringstream invalid_reserved(non_zero_reserved, std::ios::binary);
    REQUIRE_THROWS_AS(market_engine::market::read_binary(invalid_reserved), EventCodecError);
    std::istringstream trailing(bytes + "x", std::ios::binary);
    REQUIRE_THROWS_AS(market_engine::market::read_binary(trailing), EventCodecError);
}

TEST_CASE("Q16.16 maths is deterministic") {
    namespace fixed = market_engine::market::fixed_point;
    REQUIRE(fixed::from_double(1.5) == 98304);
    REQUIRE(fixed::from_double(-1.5) == -98304);
    REQUIRE(fixed::from_integer(1) == fixed::kOne);
    REQUIRE(fixed::from_integer(50000) == INT32_MAX);
    REQUIRE(fixed::from_integer(-50000) == INT32_MIN);
    REQUIRE(fixed::multiply(1, 32768) == 1);
    REQUIRE(fixed::multiply(-1, 32768) == -1);
    REQUIRE(fixed::divide(fixed::from_integer(1), fixed::from_integer(3)) == 21845);
    REQUIRE_THROWS_AS(fixed::divide(fixed::kOne, 0), fixed::FixedPointError);
    REQUIRE_THROWS_AS(fixed::from_double(std::numeric_limits<double>::infinity()), fixed::FixedPointError);
}
