// This file declares the compact result which will travel from the continuously
// running RTL side to the application side. It contains exactly the event
// control information and eight-feature payload needed to build GPU batches;
// it deliberately does not copy the full 20-level order book for every event.
#pragma once

#include <cstdint>

#include "market/order_book.hpp"

namespace market_engine::verilator {

struct RtlStreamResult {
    std::uint64_t event_index{};
    std::uint64_t timestamp_ns{};
    market::BookError error{market::BookError::None};
    market::FeatureVector features{};
};

}  // namespace market_engine::verilator
