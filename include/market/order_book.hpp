#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "market/event.hpp"

namespace market_engine::market {

inline constexpr std::size_t kBookDepth{10};

// One visible aggregated price level. A quantity of zero denotes an unused slot.
struct PriceLevel {
    std::int32_t price_ticks{};
    std::uint32_t quantity{};
    [[nodiscard]] bool operator==(const PriceLevel&) const = default;
};

// Complete fixed-depth view of both sides of the order book.
struct BookSnapshot {
    std::array<PriceLevel, kBookDepth> bids{};
    std::array<PriceLevel, kBookDepth> asks{};
    [[nodiscard]] bool operator==(const BookSnapshot&) const = default;
};

enum class BookError {
    None,
    InvalidEvent,
    MissingPrice,
    CrossedBook,
    InvariantViolation,
};

struct ApplyResult {
    BookError error{BookError::None};
    // Signed raw quantities. Bid-side flow is positive and ask-side flow negative.
    std::int64_t order_flow_delta{};
    // Aggressive trade flow: ask trades are positive and bid trades negative.
    std::int64_t trade_flow_delta{};
};

// A deterministic ten-level, aggregated order book.
class OrderBook {
public:
    explicit OrderBook(bool allow_crossed_books = false) noexcept : allow_crossed_books_(allow_crossed_books) {}

    [[nodiscard]] ApplyResult apply(const MarketEvent& event);
    [[nodiscard]] const BookSnapshot& snapshot() const noexcept { return levels_; }
    [[nodiscard]] std::optional<std::int32_t> midpoint_ticks() const noexcept;
    [[nodiscard]] std::optional<std::int32_t> microprice_ticks() const noexcept;
    [[nodiscard]] std::string check_invariants() const;

private:
    BookSnapshot levels_{};
    bool allow_crossed_books_{};
};

// Rolling event-flow and price-motion window used to calculate model features.
class FeatureWindow {
public:
    static constexpr std::size_t kWindowSize{64};

    void push(std::int64_t order_flow, std::int64_t trade_flow, std::int64_t absolute_midpoint_change) noexcept;
    void reset() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::int64_t order_flow_sum() const noexcept { return order_flow_sum_; }
    [[nodiscard]] std::int64_t trade_flow_sum() const noexcept { return trade_flow_sum_; }
    [[nodiscard]] std::int64_t absolute_midpoint_change_sum() const noexcept { return absolute_midpoint_change_sum_; }

private:
    struct Entry { std::int64_t order_flow{}; std::int64_t trade_flow{}; std::int64_t absolute_midpoint_change{}; };
    std::array<Entry, kWindowSize> entries_{};
    std::size_t next_{};
    std::size_t size_{};
    std::int64_t order_flow_sum_{};
    std::int64_t trade_flow_sum_{};
    std::int64_t absolute_midpoint_change_sum_{};
};

// Produce the eight Q16.16 features defined by the reference model.
[[nodiscard]] FeatureVector calculate_features(const OrderBook& book, const FeatureWindow& window) noexcept;
// Evaluate one eight-weight Q16.16 linear model.
[[nodiscard]] Signal evaluate_strategy(std::uint64_t timestamp_ns, std::uint64_t event_index,
                                       const FeatureVector& features, const ModelParameters& parameters) noexcept;
[[nodiscard]] std::uint64_t deterministic_checksum(const BookSnapshot& book, const FeatureVector& features,
                                                   const Signal& signal, const ModelParameters& parameters) noexcept;
[[nodiscard]] std::string to_string(BookError error);

}  // namespace market_engine::market
