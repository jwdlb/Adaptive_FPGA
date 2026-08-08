// This file declares the live feature-upload bridge. It collects RTL feature
// snapshots into 32 x 8 batches and schedules their asynchronous transfer to
// the GPU. It owns the A/B buffer scheduling, but it does not train a model or
// create OpenCL resources; GpuModel performs the actual device transfer.
#pragma once

#include <cstddef>
#include <optional>

#include "gpu/feature_buffers.hpp"
#include "gpu/gpu_protocol.hpp"

namespace market_engine::gpu {

class GpuModel;

// Reusable application-side scheduler for sending feature batches to the GPU.
// Replay mode and a future live event loop can both use the same class.
class GpuFeatureUploader {
public:
    explicit GpuFeatureUploader(GpuModel& model);

    // Poll an existing upload without waiting, then start a ready batch when
    // the GPU is free. This is called regularly by the event loop.
    void poll_and_start();

    // Add one RTL feature snapshot. Every 32 valid snapshots become one batch.
    void add_snapshot(const FeatureSnapshot& snapshot);

    // Finish already-submitted uploads before the application shuts down or
    // leaves this stream. It does not submit new market events.
    void drain();

    [[nodiscard]] std::size_t batches_submitted() const noexcept;
    [[nodiscard]] std::size_t uploads_completed() const noexcept;

private:
    GpuModel& model_;
    FeatureBatchCollector collector_;
    FeatureBufferPool buffers_;
    std::optional<std::size_t> ready_index_;
    std::optional<std::size_t> in_flight_index_;
    std::size_t batches_submitted_{};
    std::size_t uploads_completed_{};
};

}  // namespace market_engine::gpu
