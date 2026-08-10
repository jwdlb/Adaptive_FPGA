#include "app/market_data_generator.hpp"

#include <algorithm>
#include <climits>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>

#include "market/event.hpp"

namespace market_engine::app {
void generate_market_csv(const std::filesystem::path& output, const std::uint64_t seed,
                         const std::size_t event_count, MarketDataProgressCallback progress) {
    if (event_count == 0U) throw std::invalid_argument("event count must be positive");
    if (output.empty()) throw std::invalid_argument("output path must not be empty");
    std::mt19937_64 random(seed);
    std::map<std::int32_t, std::uint32_t> bids, asks;
    std::vector<market::MarketEvent> events;
    events.reserve(event_count);
    const auto uniform = [&](std::uint64_t low, std::uint64_t high) {
        return low + random() % (high - low + 1U);
    };
    for (std::size_t index = 0; index < event_count; ++index) {
        const market::Side side = uniform(0, 1) == 0 ? market::Side::Bid : market::Side::Ask;
        auto& levels = side == market::Side::Bid ? bids : asks;
        market::EventType type = market::EventType::Add;
        if (!levels.empty()) {
            const auto choice = uniform(0, 99);
            type = choice < 45 ? market::EventType::Add : choice < 60 ? market::EventType::Update :
                   choice < 80 ? market::EventType::Cancel : market::EventType::Trade;
        }
        std::int32_t price{}; std::uint32_t quantity{};
        if (type == market::EventType::Add) {
            price = static_cast<std::int32_t>(side == market::Side::Bid ? uniform(9991, 10000) : uniform(10002, 10011));
            quantity = static_cast<std::uint32_t>(uniform(1, 1000));
            const std::uint64_t sum = static_cast<std::uint64_t>(levels[price]) + quantity;
            levels[price] = static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, UINT32_MAX));
        } else {
            auto selected = levels.begin(); std::advance(selected, static_cast<std::ptrdiff_t>(uniform(0, levels.size() - 1U)));
            price = selected->first;
            if (type == market::EventType::Update) {
                quantity = static_cast<std::uint32_t>(uniform(0, 1000));
                if (quantity == 0U) levels.erase(selected); else selected->second = quantity;
            } else {
                quantity = static_cast<std::uint32_t>(uniform(1, static_cast<std::uint64_t>(selected->second) + 500U));
                if (quantity >= selected->second) levels.erase(selected); else selected->second -= quantity;
            }
        }
        events.push_back({1000U + index * 10U, type, side, price, quantity});
        if (progress && ((index + 1U) % 4096U == 0U || index + 1U == event_count)) {
            progress(index + 1U, event_count);
        }
    }
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output);
    if (!stream) throw std::runtime_error("cannot create CSV file: " + output.string());
    market::write_csv(stream, events);
    if (!stream) throw std::runtime_error("failed while writing CSV file: " + output.string());
}
}  // namespace market_engine::app
