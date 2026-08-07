#pragma once

#include <filesystem>
#include <vector>

#include "market/event.hpp"

namespace market_engine::io {

// Read canonical market events from either CSV or the versioned binary format.
[[nodiscard]] std::vector<market::MarketEvent> read_events(const std::filesystem::path& path);

}  // namespace market_engine::io
