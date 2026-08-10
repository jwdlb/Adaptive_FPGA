#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace market_engine::app {

// Generate a deterministic, valid, non-crossing CSV stream without invoking a
// shell or accepting command fragments from the dashboard. The optional
// callback receives generated-event count and total at bounded intervals.
using MarketDataProgressCallback = std::function<void(std::size_t, std::size_t)>;
void generate_market_csv(const std::filesystem::path& output,
                         std::uint64_t seed,
                         std::size_t event_count,
                         MarketDataProgressCallback progress = {});

}  // namespace market_engine::app
