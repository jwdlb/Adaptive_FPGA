#include "app/replay_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <span>
#include <vector>

#include "market/event.hpp"
#include "reference/reference_model.hpp"

namespace market_engine::app {
namespace {

// Writes to a CSV file containing the last 100 events leading up to a failure, so that the failure can be reproduced.
// Taakes in readonly list of all market events, and the position of teh event that failed.
void write_reproduction(std::span<const market::MarketEvent> events, const std::size_t failure_index) {
    const std::size_t first = failure_index > 100U ? failure_index - 100U : 0U;   // Caluclates the first of the last 100 events to record
    std::ofstream output("failure_repro.csv");  // Creates an output fiel stream to write the CSV file
    if (!output) return;   // If the file wasn't opended successfully, return early
    // Span is a lightweight view of contigous objects. `subspan` selects the event
    // range that previously used an explicit pointer and count.
    market::write_csv(output, events.subspan(first, failure_index - first + 1U));
}

}  // namespace

ReplayResult ReplayCoordinator::run_reference(const std::span<const market::MarketEvent> events,
                                              const std::optional<std::uint64_t> event_limit) const {
    reference::ReferenceModel reference_model(config_);
    ReplayResult replay{};
    const std::size_t requested = event_limit ? static_cast<std::size_t>(*event_limit) : events.size();
    const std::size_t count = std::min(events.size(), requested);
    const auto started = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < count; ++index) {
        const market::ApplyResult result = reference_model.process(events[index], index);
        if (result.error != market::BookError::None) {
            write_reproduction(events, index);
            replay.error = result.error;
            replay.failure_index = index;
            break;
        }
        ++replay.processed_events;
    }

    replay.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    replay.final_book = reference_model.book();
    replay.final_features = reference_model.features();
    replay.final_signal = reference_model.signal();
    replay.final_parameters = reference_model.parameters();
    return replay;
}

}  // namespace market_engine::app
