// This header defines the data contracts crossing the GPU boundary. It describes
// feature snapshots, 32-by-8 input batches, and complete model updates, plus the
// small validation/conversion helpers that protect the RTL hand-off. It does not
// connect to OpenCL, allocate GPU memory, or run GPU code.
#pragma once  // Prevent duplicate inclusion.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "market/event.hpp"

namespace market_engine::gpu {

// One completed RTL feature calculation for the GPU. The coordinator keeps only
// valid snapshots and later groups 32 of them into one model input example.
struct FeatureSnapshot {
    std::uint64_t event_index{};
    std::uint64_t timestamp_ns{};
    bool valid{false};
    std::array<std::int32_t, market::FeatureVector::kFeatureCount> features{};
};

// One complete replacement model from the GPU. weights[0..6] are ordinary
// feature weights; weights[7] multiplies the RTL's constant 1.0 feature, so it
// is the bias/intercept. BUY and SELL thresholds are replacement values too.
struct ModelUpdate {
    std::uint64_t update_version{};
    std::array<std::int32_t, market::FeatureVector::kFeatureCount> weights{};
    std::int32_t buy_threshold{};
    std::int32_t sell_threshold{};
};

// One GPU input example: 32 valid RTL moments, each containing eight Q16.16
// features. The nested arrays are contiguous, so batch[time][feature] is also
// the exact memory order later sent to an OpenCL input buffer.
inline constexpr std::size_t kFeatureBatchTimeSteps{32};
using FeatureBatch = std::array<std::array<std::int32_t, market::FeatureVector::kFeatureCount>,
                                kFeatureBatchTimeSteps>;

// Collect valid snapshots in arrival order. Invalid snapshots are ignored; after
// 32 valid snapshots, add() returns one batch and starts the next one.
class FeatureBatchCollector {
public:
    [[nodiscard]] std::optional<FeatureBatch> add(const FeatureSnapshot& snapshot);
    void reset() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    FeatureBatch batch_{};
    std::size_t size_{};
};

// Check an update before it reaches RTL. It must be newer than the active model
// and its BUY boundary must remain strictly above its SELL boundary. Throws
// std::invalid_argument when either rule is broken.
void validate_model_update(const ModelUpdate& update, std::uint64_t active_model_version);

// Convert a checked GPU update into the parameter type already used by the RTL
// runner. Calling this also performs the validation above.
[[nodiscard]] market::ModelParameters model_parameters_from_update(const ModelUpdate& update,
                                                                     std::uint64_t active_model_version);

}  // namespace market_engine::gpu
