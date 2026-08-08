// This file schedules feature batches on their path from RTL to the GPU.
// It collects valid 8-feature RTL snapshots into 32 x 8 batches, alternates
// between the two CPU-side A/B source buffers, and tells GpuModel when a batch
// is safe to upload. It does not run OpenCL commands itself, train a model, or
// receive new model weights; GpuModel owns the real GPU work.
#include "gpu/feature_uploader.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "gpu/gpu_model.hpp"

namespace market_engine::gpu {

// Connect this uploader to the one GpuModel that owns the OpenCL connection.
// The uploader does not create a GPU itself; it only decides when to give that
// already-created GPU the next completed 32 x 8 feature batch.
GpuFeatureUploader::GpuFeatureUploader(GpuModel& model) : model_(model) {}

// This is called repeatedly by the normal event loop. It performs two small,
// non-blocking jobs:
//
// 1. Ask whether the currently-uploading A/B batch has finished copying to GPU
//    memory. If it has, that host-side slot becomes Free for the next batch.
// 2. If no upload is running and another completed batch is Ready, start its
//    host-to-GPU copy.
//
// It never waits for the GPU. That lets incoming market events keep moving.
void GpuFeatureUploader::poll_and_start() {
    // `in_flight_index_` holds A or B only while OpenCL is copying that slot.
    // poll_feature_upload_finished() returns false while the copy is still
    // working, and true only after it has made the host slot Free again.
    if (in_flight_index_ && model_.poll_feature_upload_finished(buffers_, *in_flight_index_)) {
        // There is no longer an upload in progress. Forget which slot it was
        // and record the completed transfer for the final application report.
        in_flight_index_.reset();
        ++uploads_completed_;
    }

    // A completed 32-snapshot batch may be waiting in the other A/B slot.
    // Start it only when no previous upload is still using the GPU connection.
    if (!in_flight_index_ && ready_index_) {
        // GpuModel changes this slot from Ready -> InFlight, then gives OpenCL
        // the batch's CPU-memory pointer for an asynchronous device copy.
        model_.enqueue_feature_batch(buffers_, *ready_index_);
        // Remember that this particular A/B slot must not be filled again
        // until a later poll confirms that its GPU copy has finished.
        in_flight_index_ = *ready_index_;
        ready_index_.reset();
        ++batches_submitted_;
    }
}

// Accept one completed feature calculation from RTL. The collector keeps only
// valid snapshots and groups 32 of them into one FeatureBatch for the GPU:
//
//     32 FeatureSnapshot values  ->  one 32 x 8 FeatureBatch
//
// This function stores a completed batch in a free A/B host slot, then tries
// to begin its upload straight away. It does not wait for that upload.
void GpuFeatureUploader::add_snapshot(const FeatureSnapshot& snapshot) {
    // Most calls add just one snapshot to the partially-built batch. Only the
    // 32nd valid snapshot returns a complete batch. Invalid RTL features do
    // not count toward the 32 snapshots.
    const std::optional<FeatureBatch> completed_batch = collector_.add(snapshot);
    if (!completed_batch) return;

    // Find an unused A/B slot and immediately reserve it for CPU filling.
    // The returned index is 0 for A or 1 for B.
    const std::optional<std::size_t> free_index = buffers_.acquire_for_filling();
    if (!free_index) {
        // Both slots are still occupied: one may be uploading and the other
        // may be waiting. Stopping is safer than overwriting GPU source data.
        throw std::runtime_error(
            "GPU feature buffers are full; event source produced a third completed batch before an upload finished");
    }

    // Copy all 32 x 8 feature values into the reserved CPU-side slot, then
    // change it from Filling -> Ready for GpuModel to upload.
    buffers_.finish_filling(*free_index, *completed_batch);
    // Record that A or B is ready. poll_and_start() below will either start it
    // immediately or leave it waiting while the other slot is InFlight.
    ready_index_ = *free_index;
    poll_and_start();
}

// Finish cleanly at application shutdown or at the end of an input stream.
// Unlike poll_and_start(), this function is allowed to wait because no more
// incoming events need fast handling. It keeps the CPU batch alive until
// OpenCL confirms that it is no longer reading from it.
void GpuFeatureUploader::drain() {
    // Ten seconds prevents a broken GPU/OpenCL command from making shutdown
    // hang forever.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // Continue until neither A nor B is uploading or waiting to be uploaded.
    while (in_flight_index_ || ready_index_) {
        // First check for completion and, if possible, start any waiting batch.
        poll_and_start();
        if (in_flight_index_ || ready_index_) {
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("timed out waiting for final GPU feature upload");
            }
            // Give the operating system/GPU driver a chance to make progress
            // before immediately checking again. This is much lighter than a
            // sleep and is used only during final shutdown.
            std::this_thread::yield();
        }
    }
}

// Return how many full 32 x 8 batches have been handed to OpenCL for upload.
// `noexcept` means this simple read cannot throw an error.
std::size_t GpuFeatureUploader::batches_submitted() const noexcept { return batches_submitted_; }

// Return how many of those uploads OpenCL has confirmed as fully finished.
// This can be lower than batches_submitted() while the live application runs.
std::size_t GpuFeatureUploader::uploads_completed() const noexcept { return uploads_completed_; }

}  // namespace market_engine::gpu
