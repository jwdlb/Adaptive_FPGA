#include "market/order_book.hpp"

#include <algorithm>
#include <limits>

#include "market/fixed_point.hpp"

namespace market_engine::market {
namespace {

using Levels = std::array<PriceLevel, kBookDepth>;

// Return the mutable bid or ask level array selected by the side.
[[nodiscard]] Levels& side(BookSnapshot& book, Side value) noexcept {
    return value == Side::Bid ? book.bids : book.asks;
}
// Return a read-only view of the bid or ask level array selected by the side.
[[nodiscard]] const Levels& side(const BookSnapshot& book, Side value) noexcept {
    return value == Side::Bid ? book.bids : book.asks;
}
// Check whether left should be placed before right in this side's price order.
[[nodiscard]] bool before(Side value, std::int32_t left, std::int32_t right) noexcept {
    return value == Side::Bid ? left > right : left < right;
}

// [[nodiscard]] tells teh compiler taht teh returing boolean shoudl be used, noexcept promises that the function will not throw an exception.
// Search the selected side for a matching price and return {found, index}, using index as the insertion position when not found.
[[nodiscard]] std::pair<bool, std::size_t> lookup(const Levels& levels, Side value, std::int32_t price) noexcept {
    for (std::size_t index = 0; index < levels.size(); ++index) {
        if (levels[index].quantity == 0U || before(value, price, levels[index].price_ticks)) return {false, index};
        if (levels[index].price_ticks == price) return {true, index};
    }
    return {false, levels.size()};
}

// Remove the level at index and shift later levels one position to the left.
void remove(Levels& levels, std::size_t index) noexcept {
    for (; index + 1U < levels.size(); ++index) levels[index] = levels[index + 1U];
    levels.back() = {};
}

// Insert the level at index and shift existing levels one position to the right.
void insert(Levels& levels, std::size_t index, PriceLevel level) noexcept {
    if (index == levels.size()) return;  // The new level is worse than the visible depth.
    for (std::size_t cursor = levels.size() - 1U; cursor > index; --cursor) levels[cursor] = levels[cursor - 1U];
    levels[index] = level;
}

// This function adds two unsigned 32-bit quantities without allowing the result to overflow
[[nodiscard]] std::uint32_t saturated_add(std::uint32_t left, std::uint32_t right) noexcept {
    const auto sum = static_cast<std::uint64_t>(left) + right;
    return sum > std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<std::uint32_t>::max()
                                                             : static_cast<std::uint32_t>(sum);
}

// This converts the order-book side into a numeric sign (+1 for bids, -1 for asks) that can be used to calculate order-flow and trade-flow deltas.
[[nodiscard]] std::int64_t side_sign(Side value) noexcept { return value == Side::Bid ? 1 : -1; }

// This function checks whether the order book is internally valid.
// It returns: - An error message if something is invalid. - An empty string if everything is valid.
[[nodiscard]] std::string invariants(const BookSnapshot& book, bool allow_crossed) {
    for (const auto value : {Side::Bid, Side::Ask}) {
        const auto& levels = side(book, value);
        // Active levels must be contiguous; once an empty slot appears, no later slot may be used.
        bool saw_empty = false;
        for (std::size_t index = 0; index < levels.size(); ++index) {
            const auto& level = levels[index];
            if (level.quantity == 0U) {
                // An unused slot must have no price as well as zero quantity.
                if (level.price_ticks != 0) return "unused level has a price";
                saw_empty = true;
                continue;
            }
            // A populated level cannot appear after an unused slot.
            if (saw_empty) return "used level follows an unused level";
            // Every populated level must use a strictly positive price.
            if (level.price_ticks <= 0) return "used level has a non-positive price";
            // Bids must descend by price; asks must ascend by price, with no duplicates.
            if (index > 0U && levels[index - 1U].quantity != 0U && !before(value, levels[index - 1U].price_ticks, level.price_ticks)) {
                return value == Side::Bid ? "bids are not strictly descending" : "asks are not strictly ascending";
            }
        }
    }
    // Unless explicitly permitted, the best bid must remain strictly below the best ask.
    if (!allow_crossed && book.bids[0].quantity != 0U && book.asks[0].quantity != 0U &&
        book.bids[0].price_ticks >= book.asks[0].price_ticks) return "book is crossed";
    return {};
}

// This calculates a signed Q16.16 ratio: - result = numerator / denominator    
// but stores it as: - result = (numerator / denominator) × 65,536
[[nodiscard]] fixed_point::Value ratio(std::int64_t numerator, std::int64_t denominator) noexcept {
    if (denominator <= 0) return 0;
    return fixed_point::saturate(fixed_point::rounded_divide(numerator * fixed_point::kScale, denominator));
}

// This mixes a new 64-bit value into an existing hash
// This function helps build the deterministic checksum for the book, features, signal, and model parameters. It is useful for detecting differences, but it is not encryption or a security hash
[[nodiscard]] std::uint64_t fnv_mix(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xffU;
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

std::string to_string(BookError error) {
    switch (error) {
        case BookError::None: return "no book error";
        case BookError::InvalidEvent: return "invalid event";
        case BookError::MissingPrice: return "event refers to a missing price";
        case BookError::CrossedBook: return "event would cross the book";
        case BookError::InvariantViolation: return "order-book invariant violation";
    }
    return "unknown book error";
}

ApplyResult OrderBook::apply(const MarketEvent& event) {
    if (validate_event(event) != EventValidationError::None) return {.error = BookError::InvalidEvent};
    BookSnapshot candidate = levels_;  // Do all work on a copy: failures never mutate the live book.
    auto& levels = side(candidate, event.side);
    const auto [found, index] = lookup(levels, event.side, event.price_ticks);
    ApplyResult result;
    const auto sign = side_sign(event.side);

    switch (event.type) {
        case EventType::Add:
            if (found) {
                const auto old = levels[index].quantity;
                levels[index].quantity = saturated_add(old, event.quantity);
                result.order_flow_delta = sign * static_cast<std::int64_t>(levels[index].quantity - old);
            } else {
                insert(levels, index, {event.price_ticks, event.quantity});
                if (index < kBookDepth) result.order_flow_delta = sign * static_cast<std::int64_t>(event.quantity);
            }
            break;
        case EventType::Update:
            if (!found) return {.error = BookError::MissingPrice};
            result.order_flow_delta = sign * (static_cast<std::int64_t>(event.quantity) - levels[index].quantity);
            if (event.quantity == 0U) remove(levels, index);
            else levels[index].quantity = event.quantity;
            break;
        case EventType::Cancel:
        case EventType::Trade: {
            if (!found) return {.error = BookError::MissingPrice};
            const auto removed = std::min(levels[index].quantity, event.quantity);
            levels[index].quantity -= removed;
            result.order_flow_delta = -sign * static_cast<std::int64_t>(removed);
            if (event.type == EventType::Trade) result.trade_flow_delta = event.side == Side::Ask ? removed : -static_cast<std::int64_t>(removed);
            if (levels[index].quantity == 0U) remove(levels, index);
            break;
        }
        default: return {.error = BookError::InvalidEvent};
    }

    const auto invariant_error = invariants(candidate, allow_crossed_books_);
    if (!invariant_error.empty()) {
        result.error = invariant_error == "book is crossed" ? BookError::CrossedBook : BookError::InvariantViolation;
        return result;
    }
    levels_ = candidate;
    return result;
}

// This function calculates the midpoint price between the best bid and best ask.
std::optional<std::int32_t> OrderBook::midpoint_ticks() const noexcept {
    const auto& bid = levels_.bids.front();
    const auto& ask = levels_.asks.front();
    if (bid.quantity == 0U || ask.quantity == 0U) return std::nullopt;
    return static_cast<std::int32_t>((static_cast<std::int64_t>(bid.price_ticks) + ask.price_ticks) / 2);
}

// This function calculates the quantity-weighted midpoint, called the microprice.
std::optional<std::int32_t> OrderBook::microprice_ticks() const noexcept {
    const auto& bid = levels_.bids.front();
    const auto& ask = levels_.asks.front();
    const auto total = static_cast<std::uint64_t>(bid.quantity) + ask.quantity;
    if (bid.quantity == 0U || ask.quantity == 0U || total == 0U) return std::nullopt;
    const auto weighted = static_cast<std::int64_t>(ask.price_ticks) * bid.quantity + static_cast<std::int64_t>(bid.price_ticks) * ask.quantity;
    return static_cast<std::int32_t>(fixed_point::rounded_divide(weighted, static_cast<std::int64_t>(total)));
}

std::string OrderBook::check_invariants() const { return invariants(levels_, allow_crossed_books_); }

// This function adds one event’s measurements to the rolling 64-event window
// If teh widow is already full, the oldest event is removed automatically.
void FeatureWindow::push(std::int64_t order_flow, std::int64_t trade_flow, std::int64_t absolute_midpoint_change) noexcept {
    if (size_ == kWindowSize) {
        const auto& old = entries_[next_];
        order_flow_sum_ -= old.order_flow;
        trade_flow_sum_ -= old.trade_flow;
        absolute_midpoint_change_sum_ -= old.absolute_midpoint_change;
    } else {
        ++size_;
    }
    entries_[next_] = {order_flow, trade_flow, absolute_midpoint_change};
    order_flow_sum_ += order_flow;
    trade_flow_sum_ += trade_flow;
    absolute_midpoint_change_sum_ += absolute_midpoint_change;
    next_ = (next_ + 1U) % kWindowSize;
}

void FeatureWindow::reset() noexcept { *this = {}; }

FeatureVector calculate_features(const OrderBook& book, const FeatureWindow& window) noexcept {
    FeatureVector result;
    const auto& snapshot = book.snapshot();
    const auto& bid = snapshot.bids.front();
    const auto& ask = snapshot.asks.front();
    const auto midpoint = book.midpoint_ticks();
    const auto microprice = book.microprice_ticks();
    if (bid.quantity == 0U || ask.quantity == 0U || !midpoint || !microprice || *midpoint <= 0) return result;

    std::int64_t bid_total = 0;
    std::int64_t ask_total = 0;
    for (std::size_t index = 0; index < kBookDepth; ++index) {
        bid_total += snapshot.bids[index].quantity;
        ask_total += snapshot.asks[index].quantity;
    }
    const auto l1_total = static_cast<std::int64_t>(bid.quantity) + ask.quantity;
    const auto l10_total = bid_total + ask_total;
    result.values[0] = ratio(static_cast<std::int64_t>(ask.price_ticks) - bid.price_ticks, *midpoint);
    result.values[1] = ratio(static_cast<std::int64_t>(bid.quantity) - ask.quantity, l1_total);
    result.values[2] = ratio(bid_total - ask_total, l10_total);
    result.values[3] = ratio(static_cast<std::int64_t>(*microprice) - *midpoint, *midpoint);
    result.values[4] = ratio(window.order_flow_sum(), l10_total);
    result.values[5] = ratio(window.trade_flow_sum(), l10_total);
    result.values[6] = ratio(window.absolute_midpoint_change_sum(), static_cast<std::int64_t>(*midpoint) * static_cast<std::int64_t>(std::max<std::size_t>(1U, window.size())));
    result.values[7] = fixed_point::kOne;
    result.valid = true;
    return result;
}

Signal evaluate_strategy(std::uint64_t timestamp_ns, std::uint64_t event_index, const FeatureVector& features,
                         const ModelParameters& parameters) noexcept {
    Signal result{.timestamp_ns = timestamp_ns, .event_index = event_index, .model_version = parameters.model_version};
    if (!features.valid) return result;
    std::int64_t score = 0;
    for (std::size_t index = 0; index < FeatureVector::kFeatureCount; ++index) {
        score += fixed_point::rounded_divide(static_cast<std::int64_t>(features.values[index]) * parameters.weights[index], fixed_point::kScale);
    }
    result.score = fixed_point::saturate(score);
    result.action = result.score > parameters.buy_threshold ? Action::Buy :
                    result.score < parameters.sell_threshold ? Action::Sell : Action::Hold;
    result.valid = true;
    return result;
}

// This function creates a deterministic 64-bit checksum from the complete current model state.
std::uint64_t deterministic_checksum(const BookSnapshot& book, const FeatureVector& features, const Signal& signal,
                                     const ModelParameters& parameters) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& levels : {book.bids, book.asks}) for (const auto& level : levels) {
        hash = fnv_mix(hash, static_cast<std::uint32_t>(level.price_ticks));
        hash = fnv_mix(hash, level.quantity);
    }
    for (const auto value : features.values) hash = fnv_mix(hash, static_cast<std::uint32_t>(value));
    hash = fnv_mix(hash, features.valid);
    hash = fnv_mix(hash, signal.timestamp_ns);
    hash = fnv_mix(hash, signal.event_index);
    hash = fnv_mix(hash, static_cast<std::uint32_t>(signal.score));
    hash = fnv_mix(hash, static_cast<std::uint8_t>(signal.action));
    hash = fnv_mix(hash, signal.valid);
    hash = fnv_mix(hash, signal.model_version);
    for (const auto weight : parameters.weights) hash = fnv_mix(hash, static_cast<std::uint32_t>(weight));
    hash = fnv_mix(hash, static_cast<std::uint32_t>(parameters.buy_threshold));
    hash = fnv_mix(hash, static_cast<std::uint32_t>(parameters.sell_threshold));
    hash = fnv_mix(hash, parameters.model_version);
    return fnv_mix(hash, parameters.update_count);
}

}  // namespace market_engine::market
