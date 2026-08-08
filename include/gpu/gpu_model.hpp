#pragma once  // Prevent duplicate inclusion.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/opencl_devices.hpp"

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

// Owns the reusable OpenCL connection for a selected GPU. It deliberately knows
// nothing about features, learning, or RTL: those contracts arrive in Phase 6.5.
class GpuModel {
public:
    // Select a GPU and create its OpenCL context, queue, compiled smoke kernel,
    // input/output buffers, and completion-event storage.
    explicit GpuModel(std::optional<std::uint32_t> gpu_index = std::nullopt,
                      std::optional<std::string_view> gpu_name = std::nullopt);
    ~GpuModel();

    GpuModel(const GpuModel&) = delete;
    GpuModel& operator=(const GpuModel&) = delete;
    GpuModel(GpuModel&&) noexcept;
    GpuModel& operator=(GpuModel&&) noexcept;

    [[nodiscard]] const app::OpenclDeviceInfo& device() const noexcept;

    // Send values to the GPU, run the tiny double_values kernel, and wait only
    // for this operation's completion event before returning the result.
    [[nodiscard]] std::vector<float> double_values(std::span<const float> input);

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
