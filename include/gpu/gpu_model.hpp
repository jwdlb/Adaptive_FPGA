// This header declares the GPU runtime controller. GpuModel selects a GPU, owns
// its OpenCL context/queue/program/buffers/events, uploads feature batches, and
// runs the current smoke operation. It supplies the GPU machinery; the data
// shapes and model-update rules live in gpu_protocol.hpp.
#pragma once  // Prevent duplicate inclusion.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/opencl_devices.hpp"
#include "gpu/gpu_protocol.hpp"

namespace market_engine::gpu {

// Result of the small OpenCL health check. A skipped result means the current
// machine has no selectable GPU; it is not reported as a successful GPU run.
enum class GpuSmokeTestStatus {
    passed,
    skipped,
    failed,
};

struct GpuSmokeTestResult {
    GpuSmokeTestStatus status{GpuSmokeTestStatus::failed};
    std::string message{};
    std::vector<float> output{};
    std::optional<app::OpenclDeviceInfo> device{};
};

// Two mapped OpenCL regions for one labelled Phase 7 training batch. Features
// remain exactly `[sample][8]`; labels are a separate one-value-per-sample
// training-answer buffer and are never fed into the RTL strategy as features.
struct MappedTrainingBatch {
    std::span<std::int32_t> features{};
    std::span<std::int32_t> labels{};
};

// Owns the reusable OpenCL connection for a selected GPU. It deliberately
// knows nothing about the learner or RTL.
class GpuModel {
public:
    // Select a GPU and create its OpenCL context, queue, compiled smoke kernel,
    // input/output buffers, and completion-event storage.
    explicit GpuModel(std::optional<std::uint32_t> gpu_index = std::nullopt,
                      std::optional<std::string_view> gpu_name = std::nullopt);
    // Destroy the GPU controller and release its OpenCL context, queue,
    // kernels, buffers, and events. The GPU connection is uniquely owned by
    // this object, so destruction must clean up those resources exactly once.
    ~GpuModel();

    // Copying is disabled because two objects must not pretend to own the same
    // GPU connection. This prevents double cleanup and ambiguous GPU ownership.
    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;

    // Moving transfers the one GPU connection into a new object. The old object
    // is left empty, so the OpenCL resources still have only one owner.
    GpuModel(GpuModel&&) noexcept;
    // Move assignment first gives up this object's current connection, then
    // takes ownership of the connection from the other object.
    GpuModel& operator=(GpuModel&&) noexcept;

    [[nodiscard]] const app::OpenclDeviceInfo& device() const noexcept;

    // Send values to the GPU, run the tiny double_values kernel, and wait only
    // for this operation's completion event before returning the result.
    [[nodiscard]] std::vector<float> double_values(std::span<const float> input);

    // Map one configurable `[sample][8 features]` OpenCL input buffer for C++
    // writes. GpuWorker fills this memory directly from SPSC results; there is no
    // intermediate host-side upload buffer. The returned span contains rows * 8 Q16.16
    // values and remains valid only until submit_phase6_model_update() or
    // discard_stream_feature_rows() is called.
    [[nodiscard]] std::span<std::int32_t> map_stream_feature_rows(std::size_t rows);
    // Unmap the completed streaming input, run the deterministic Phase 6 kernel,
    // and start an asynchronous readback of one complete ModelUpdate. `version`
    // labels this whole replacement model; Phase 7 will replace the kernel with
    // real learning while preserving this method's input/output contract.
    void submit_phase6_model_update(std::uint64_t version);
    // Return a completed Phase 6 update only after its OpenCL readback event has
    // finished. An empty optional means GPU work is still running.
    [[nodiscard]] std::optional<ModelUpdate> poll_phase6_model_update();
    // Discard a partially filled mapped input during orderly shutdown. No GPU
    // kernel is run and the same buffer becomes available for a future batch.
    void discard_stream_feature_rows();

    // Map a configurable labelled training batch. GpuWorker fills the existing
    // eight-feature layout and a separate Q16.16 BUY/HOLD/SELL label per row.
    [[nodiscard]] MappedTrainingBatch map_training_batch(std::size_t rows);
    // Unmap the completed training batch and have the GPU fine-tune its
    // persistent linear weights and BUY/SELL thresholds. The GPU returns a full
    // replacement ModelUpdate through the existing poll/mailbox path.
    void submit_training_batch(std::uint64_t version, std::int32_t learning_rate_q16);
    [[nodiscard]] std::optional<ModelUpdate> poll_training_update();
    void discard_training_batch();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Run the [1, 2, 3] -> [2, 4, 6] health check. Missing OpenCL/GPU hardware is a
// clear skipped outcome; an available GPU with incorrect output is a failure.
[[nodiscard]] GpuSmokeTestResult run_gpu_smoke_test(
    std::optional<std::uint32_t> gpu_index = std::nullopt,
    std::optional<std::string_view> gpu_name = std::nullopt);

}  // namespace market_engine::gpu
