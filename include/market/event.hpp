#pragma once  // Prevent duplicate inclusion.

#include <array>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace market_engine::market {

// Types of market events supported by the input data.
enum class EventType : std::uint8_t { Add = 0, Update = 1, Cancel = 2, Trade = 3 };
// The side of the order book affected by an event.
enum class Side : std::uint8_t { Bid = 0, Ask = 1 };
// Trading action selected by the model.
enum class Action : std::uint8_t { Hold = 0, Buy = 1, Sell = 2 };

// One normalized market event.
struct MarketEvent {
    std::uint64_t timestamp_ns{};
    EventType type{};
    Side side{};
    std::int32_t price_ticks{};
    std::uint32_t quantity{};
    [[nodiscard]] bool operator==(const MarketEvent&) const = default;
};

// Fixed-point input features calculated from market events.
struct FeatureVector {
    static constexpr std::size_t kFeatureCount{8};
    std::array<std::int32_t, kFeatureCount> values{};
    bool valid{false};
};

// Model output associated with a particular event.
struct Signal {
    std::uint64_t timestamp_ns{};
    std::uint64_t event_index{};
    std::int32_t score{};
    Action action{Action::Hold};
    bool valid{false};
    std::uint64_t model_version{};
};

// Weights and thresholds used by the model.
struct ModelParameters {
    std::array<std::int32_t, FeatureVector::kFeatureCount> weights{};
    std::int32_t buy_threshold{};
    std::int32_t sell_threshold{};
    std::uint64_t model_version{};
    std::uint64_t update_count{};
};

// Possible reasons why a market event is invalid.
enum class EventValidationError { None, InvalidEventType, InvalidSide, NonPositivePrice, ZeroQuantity };

// Exception thrown when CSV or binary event data cannot be encoded or decoded.
class EventCodecError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Validate the fields of one event without throwing.
[[nodiscard]] EventValidationError validate_event(const MarketEvent& event) noexcept;
// Convert event types, sides, actions, and errors to readable text.
[[nodiscard]] std::string to_string(EventType type);
[[nodiscard]] std::string to_string(Side side);
[[nodiscard]] std::string to_string(Action action);
[[nodiscard]] std::string to_string(EventValidationError error);
// Read and write the human-readable CSV representation.
[[nodiscard]] std::vector<MarketEvent> read_csv(std::istream& input);
void write_csv(std::ostream& output, std::span<const MarketEvent> events);
// Read and write the strict explicit-little-endian binary representation.
[[nodiscard]] std::vector<MarketEvent> read_binary(std::istream& input);
void write_binary(std::ostream& output, std::span<const MarketEvent> events);

}  // namespace market_engine::market
