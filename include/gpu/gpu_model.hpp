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
#include "gpu/feature_buffers.hpp"

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

// Owns the reusable OpenCL connection for a selected GPU and two device-side
// FeatureBatch buffers. It deliberately knows nothing about the learner or RTL.
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

    // Mark one Ready host batch InFlight before giving its pointer to OpenCL,
    // then enqueue a non-blocking copy into the matching device buffer. This
    // keeps the CPU batch protected for the whole asynchronous transfer.
    void enqueue_feature_batch(FeatureBufferPool& host_buffers, std::size_t buffer_index);
    // Poll one non-blocking host-to-GPU feature upload. Return false while the
    // copy is queued/running; once its event completes, release that event and
    // make the paired host slot Free. A later learner will extend this tracking
    // so the slot is freed only after the learner finishes reading it too.
    [[nodiscard]] bool poll_feature_upload_finished(FeatureBufferPool& host_buffers, std::size_t buffer_index);

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
