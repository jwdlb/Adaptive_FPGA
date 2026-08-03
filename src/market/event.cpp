#include "market/event.hpp"

#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <string_view>

namespace market_engine::market {
namespace {
constexpr std::array<char, 4> kMagic{{'M', 'K', 'T', '1'}};
constexpr std::uint16_t kVersion{1};
constexpr std::uint16_t kHeaderSize{16};
constexpr std::uint32_t kRecordSize{20};
constexpr std::string_view kHeader{"timestamp_ns,event_type,side,price_ticks,quantity"};

[[nodiscard]] bool valid(EventType value) noexcept {
    return value == EventType::Add || value == EventType::Update || value == EventType::Cancel || value == EventType::Trade;
}
[[nodiscard]] bool valid(Side value) noexcept { return value == Side::Bid || value == Side::Ask; }
[[nodiscard]] std::string line_error(std::size_t line, std::string_view message) {
    return "CSV line " + std::to_string(line) + ": " + std::string(message);
}
[[nodiscard]] std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t start = 0;
    while (true) {
        const auto comma = line.find(',', start);
        result.push_back(line.substr(start, comma == std::string_view::npos ? line.size() - start : comma - start));
        if (comma == std::string_view::npos) return result;
        start = comma + 1;
    }
}
template <typename T>
[[nodiscard]] T parse_number(std::string_view value, std::size_t line, std::string_view field) {
    T parsed{};
    const auto [last, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || last != value.data() + value.size()) {
        throw EventCodecError(line_error(line, "invalid " + std::string(field)));
    }
    return parsed;
}
[[nodiscard]] EventType parse_type(std::string_view value, std::size_t line) {
    if (value == "Add") return EventType::Add;
    if (value == "Update") return EventType::Update;
    if (value == "Cancel") return EventType::Cancel;
    if (value == "Trade") return EventType::Trade;
    throw EventCodecError(line_error(line, "invalid event_type"));
}
[[nodiscard]] Side parse_side(std::string_view value, std::size_t line) {
    if (value == "Bid") return Side::Bid;
    if (value == "Ask") return Side::Ask;
    throw EventCodecError(line_error(line, "invalid side"));
}
void require_valid(const MarketEvent& event, std::string_view context) {
    const auto error = validate_event(event);
    if (error != EventValidationError::None) throw EventCodecError(std::string(context) + ": " + to_string(error));
}
void write_u16(std::ostream& out, std::uint16_t value) { for (int i = 0; i < 2; ++i) out.put(static_cast<char>((value >> (8 * i)) & 0xffU)); }
void write_u32(std::ostream& out, std::uint32_t value) { for (int i = 0; i < 4; ++i) out.put(static_cast<char>((value >> (8 * i)) & 0xffU)); }
void write_u64(std::ostream& out, std::uint64_t value) { for (int i = 0; i < 8; ++i) out.put(static_cast<char>((value >> (8 * i)) & 0xffU)); }
template <typename T>
[[nodiscard]] T read_le(std::istream& in, int bytes, std::string_view name) {
    T value{};
    for (int i = 0; i < bytes; ++i) {
        const int next = in.get();
        if (next == std::char_traits<char>::eof()) throw EventCodecError("truncated binary " + std::string(name));
        value |= static_cast<T>(static_cast<unsigned char>(next)) << (8 * i);
    }
    return value;
}
void check_output(const std::ostream& out) { if (!out) throw EventCodecError("failed to write event output"); }
}  // namespace

EventValidationError validate_event(const MarketEvent& event) noexcept {
    if (!valid(event.type)) return EventValidationError::InvalidEventType;
    if (!valid(event.side)) return EventValidationError::InvalidSide;
    if (event.price_ticks <= 0) return EventValidationError::NonPositivePrice;
    if (event.quantity == 0U && event.type != EventType::Update) return EventValidationError::ZeroQuantity;
    return EventValidationError::None;
}
std::string to_string(EventType type) {
    switch (type) { case EventType::Add: return "Add"; case EventType::Update: return "Update"; case EventType::Cancel: return "Cancel"; case EventType::Trade: return "Trade"; }
    return "InvalidEventType";
}
std::string to_string(Side side) {
    switch (side) { case Side::Bid: return "Bid"; case Side::Ask: return "Ask"; }
    return "InvalidSide";
}
std::string to_string(Action action) {
    switch (action) { case Action::Hold: return "Hold"; case Action::Buy: return "Buy"; case Action::Sell: return "Sell"; }
    return "InvalidAction";
}
std::string to_string(EventValidationError error) {
    switch (error) {
        case EventValidationError::None: return "no validation error";
        case EventValidationError::InvalidEventType: return "invalid event type";
        case EventValidationError::InvalidSide: return "invalid side";
        case EventValidationError::NonPositivePrice: return "price_ticks must be positive";
        case EventValidationError::ZeroQuantity: return "quantity must be non-zero except for Update";
    }
    return "unknown validation error";
}

std::vector<MarketEvent> read_csv(std::istream& input) {
    std::string line;
    if (!std::getline(input, line) || line != kHeader) throw EventCodecError("CSV header must be " + std::string(kHeader));
    std::vector<MarketEvent> events;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        const auto fields = split(line);
        if (line.empty() || fields.size() != 5U) throw EventCodecError(line_error(line_number, "expected five fields"));
        const MarketEvent event{
            .timestamp_ns = parse_number<std::uint64_t>(fields[0], line_number, "timestamp_ns"),
            .type = parse_type(fields[1], line_number),
            .side = parse_side(fields[2], line_number),
            .price_ticks = parse_number<std::int32_t>(fields[3], line_number, "price_ticks"),
            .quantity = parse_number<std::uint32_t>(fields[4], line_number, "quantity"),
        };
        require_valid(event, line_error(line_number, "invalid event"));
        events.push_back(event);
    }
    if (!input.eof()) throw EventCodecError("failed while reading CSV input");
    return events;
}
void write_csv(std::ostream& output, std::span<const MarketEvent> events) {
    output << kHeader << '\n';
    for (const auto& event : events) {
        require_valid(event, "cannot write invalid CSV event");
        output << event.timestamp_ns << ',' << to_string(event.type) << ',' << to_string(event.side) << ',' << event.price_ticks << ',' << event.quantity << '\n';
    }
    check_output(output);
}

void write_binary(std::ostream& output, std::span<const MarketEvent> events) {
    if (events.size() > std::numeric_limits<std::uint32_t>::max()) throw EventCodecError("too many events for binary v1");
    for (const auto& event : events) require_valid(event, "cannot write invalid binary event");
    output.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    write_u16(output, kVersion); write_u16(output, kHeaderSize); write_u32(output, kRecordSize); write_u32(output, static_cast<std::uint32_t>(events.size()));
    for (const auto& event : events) {
        write_u64(output, event.timestamp_ns);
        output.put(static_cast<char>(event.type)); output.put(static_cast<char>(event.side)); write_u16(output, 0U);
        write_u32(output, static_cast<std::uint32_t>(event.price_ticks)); write_u32(output, event.quantity);
    }
    check_output(output);
}
std::vector<MarketEvent> read_binary(std::istream& input) {
    std::array<char, 4> magic{};
    input.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (input.gcount() != static_cast<std::streamsize>(magic.size()) || magic != kMagic) throw EventCodecError("invalid or truncated binary magic");
    const auto version = read_le<std::uint16_t>(input, 2, "version");
    const auto header_size = read_le<std::uint16_t>(input, 2, "header size");
    const auto record_size = read_le<std::uint32_t>(input, 4, "record size");
    const auto count = read_le<std::uint32_t>(input, 4, "record count");
    if (version != kVersion || header_size != kHeaderSize || record_size != kRecordSize) throw EventCodecError("unsupported binary layout");
    std::vector<MarketEvent> events; events.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto timestamp = read_le<std::uint64_t>(input, 8, "timestamp");
        const int type = input.get(); const int side = input.get();
        if (type == std::char_traits<char>::eof() || side == std::char_traits<char>::eof()) throw EventCodecError("truncated binary event");
        const auto reserved = read_le<std::uint16_t>(input, 2, "reserved");
        if (reserved != 0U) throw EventCodecError("binary event " + std::to_string(index) + " has non-zero reserved bytes");
        const auto price_bits = read_le<std::uint32_t>(input, 4, "price");
        const MarketEvent event{timestamp, static_cast<EventType>(static_cast<std::uint8_t>(type)), static_cast<Side>(static_cast<std::uint8_t>(side)), static_cast<std::int32_t>(price_bits), read_le<std::uint32_t>(input, 4, "quantity")};
        require_valid(event, "binary event " + std::to_string(index));
        events.push_back(event);
    }
    if (input.peek() != std::char_traits<char>::eof()) throw EventCodecError("binary input contains trailing bytes");
    return events;
}
}  // namespace market_engine::market
