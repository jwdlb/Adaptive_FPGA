#include "replay_coordinator.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <span>
#include <vector>

#include "gpu/feature_uploader.hpp"
#include "gpu/gpu_model.hpp"
#include "gpu/gpu_protocol.hpp"
#include "market/event.hpp"
#include "reference/reference_model.hpp"
#include "verilator/verilator_runner.hpp"

namespace market_engine::test_support {
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

// Compare every bid and ask level in two order-book snapshots.
// Returns no value when they match, or a message identifying the first mismatch.
[[nodiscard]] std::optional<std::string> compare_book(const market::BookSnapshot& expected,
                                                       const market::BookSnapshot& actual) {
    for (std::size_t index = 0; index < market::kBookDepth; ++index) {
        if (expected.bids[index] != actual.bids[index]) return "bid book level " + std::to_string(index) + " differs";
        if (expected.asks[index] != actual.asks[index]) return "ask book level " + std::to_string(index) + " differs";
    }
    return std::nullopt;
}

// Compare the complete successful-event state from the C++ reference model
// with the state reported by the simulated RTL pipeline.
// Returns no value when both implementations produced the same result.
[[nodiscard]] std::optional<std::string> compare_successful_event(
    const reference::ReferenceModel& reference_model, const verilator::RtlSnapshot& rtl) {
    // Check the resulting order book before comparing derived values.
    if (const auto book_difference = compare_book(reference_model.book(), rtl.book)) return book_difference;
    const market::FeatureVector& expected_features = reference_model.features();
    // Features must have the same validity flag and all eight values must match.
    if (expected_features.valid != rtl.features.valid || expected_features.values != rtl.features.values) {
        return "feature vector differs";
    }
    const market::Signal& expected_signal = reference_model.signal();
    // Check every part of the model decision, including which event and model made it.
    if (expected_signal.timestamp_ns != rtl.signal.timestamp_ns || expected_signal.event_index != rtl.signal.event_index ||
        expected_signal.score != rtl.signal.score || expected_signal.action != rtl.signal.action ||
        expected_signal.valid != rtl.signal.valid || expected_signal.model_version != rtl.signal.model_version) {
        return "signal differs";
    }
    return std::nullopt;
}

// Compare the immediate outcome of applying one event in both implementations.
// This covers rejection/acceptance and the flow deltas caused by the event.
[[nodiscard]] std::optional<std::string> compare_event_result(const market::ApplyResult& expected,
                                                               const verilator::RtlSnapshot& actual) {
    if (expected.error != actual.error) return "event error differs";
    if (expected.order_flow_delta != actual.order_flow_delta) return "order-flow delta differs";
    if (expected.trade_flow_delta != actual.trade_flow_delta) return "trade-flow delta differs";
    return std::nullopt;
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

// Replay market events through both the C++ reference model and simulated RTL.
// Stop at the first difference, save a reproduction input, and return the
// completed work, final state, and simulation measurements.
ReplayResult ReplayCoordinator::run_verilator_check(const std::span<const market::MarketEvent> events,
                                                     const std::optional<std::uint64_t> event_limit,
                                                     gpu::GpuModel* const gpu_model) const {
#if MARKET_ENGINE_VERILATOR_AVAILABLE
    // The C++ reference model is the expected answer; the runner drives the RTL.
    reference::ReferenceModel reference_model(config_);
    verilator::VerilatorRunner rtl_runner(config_.clock_period_ns);
    // Start both implementations with the same active strategy parameters.
    rtl_runner.write_model_parameters(reference_model.parameters());

    // Keep the upload coordinator optional so ordinary RTL/reference checking
    // continues to work on machines without a GPU.
    std::optional<gpu::GpuFeatureUploader> gpu_uploader;
    if (gpu_model != nullptr) gpu_uploader.emplace(*gpu_model);

    ReplayResult replay{};
    // Use every supplied event unless the caller requested a smaller limit.
    const std::size_t requested = event_limit ? static_cast<std::size_t>(*event_limit) : events.size();
    const std::size_t count = std::min(events.size(), requested);
    const auto started = std::chrono::steady_clock::now();

    for (std::size_t index = 0; index < count; ++index) {
        // Poll once per replay event, without blocking, before producing more
        // feature data. This lets a finished A upload make room for B.
        if (gpu_uploader) gpu_uploader->poll_and_start();
        // Process the same event in software and simulated hardware.
        const market::ApplyResult expected = reference_model.process(events[index], index);
        const verilator::RtlSnapshot actual = rtl_runner.process(events[index]);

        // Compare the direct event outcome. For valid events, also compare all
        // derived state; rejected events only need to preserve the order book.
        const auto result_difference = compare_event_result(expected, actual);
        const auto state_difference = expected.error == market::BookError::None
            ? compare_successful_event(reference_model, actual) : compare_book(reference_model.book(), actual.book);
        if (result_difference || state_difference) {
            // Save the shortest input that reproduces the RTL/reference mismatch.
            write_reproduction(events, index);
            replay.error = market::BookError::InvariantViolation;
            replay.failure_index = index;
            replay.divergence_message = result_difference ? *result_difference : *state_difference;
            break;
        }
        if (expected.error != market::BookError::None) {
            // Both implementations agree on a rejected input; record it and stop.
            write_reproduction(events, index);
            replay.error = expected.error;
            replay.failure_index = index;
            break;
        }
        if (gpu_uploader) {
            // The RTL snapshot is the actual FPGA-side feature source. Its
            // feature validity determines whether it contributes to a batch.
            gpu_uploader->add_snapshot({
                .event_index = actual.signal.event_index,
                .timestamp_ns = actual.signal.timestamp_ns,
                .valid = actual.features.valid,
                .features = actual.features.values,
            });
        }
        // Count only events that both implementations accepted successfully.
        ++replay.processed_events;
    }

    // The steady-state loop above never waits. Draining happens only once the
    // replay has ended, preserving the host source batch until its upload ends.
    if (gpu_uploader) {
        gpu_uploader->drain();
        replay.gpu_feature_batches_submitted = gpu_uploader->batches_submitted();
        replay.gpu_feature_uploads_completed = gpu_uploader->uploads_completed();
    }

    // Return the reference-model state, which is known to match the RTL unless
    // the loop stopped because it found a difference.
    replay.elapsed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    replay.final_book = reference_model.book();
    replay.final_features = reference_model.features();
    replay.final_signal = reference_model.signal();
    replay.final_parameters = reference_model.parameters();
    const verilator::RunnerMetrics metrics = rtl_runner.metrics();
    replay.rtl_cycles = metrics.cycles;
    replay.rtl_wall_seconds = metrics.wall_seconds;
    return replay;
#else
    // Avoid unused-parameter warnings in builds that do not include Verilator.
    static_cast<void>(events);
    static_cast<void>(event_limit);
    static_cast<void>(gpu_model);
    throw std::runtime_error("--verilator-check requires a build with Verilator available");
#endif
}

}  // namespace market_engine::test_support
