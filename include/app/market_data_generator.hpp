#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace market_engine::app {

// Generate a deterministic, valid, non-crossing CSV stream without invoking a
// shell or accepting command fragments from the dashboard.
void generate_market_csv(const std::filesystem::path& output,
                         std::uint64_t seed,
                         std::size_t event_count);

}  // namespace market_engine::app
