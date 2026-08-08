#pragma once  // Prevent duplicate inclusion.

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace market_engine::app {

// A human-readable description of one OpenCL device. gpu_index is set only for GPUs
// and is the number accepted by --gpu-index.
struct OpenclDeviceInfo {
    std::uint32_t platform_index{};
    std::uint32_t device_index{};
    std::optional<std::uint32_t> gpu_index{};
    std::string platform_name{};
    std::string platform_vendor{};
    std::string platform_version{};
    std::string name{};
    std::string vendor{};
    std::string version{};
    std::string type{};
    std::uint64_t global_memory_bytes{};
    std::uint32_t max_compute_units{};
};

// Raised when the requested GPU cannot be chosen. This is separate from a normal
// configuration error because it describes the machine's OpenCL setup.
class OpenclSelectionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Return every OpenCL device that the machine exposes. The returned information is
// deliberately data-only, so later GPU code can create its own OpenCL context.
[[nodiscard]] std::vector<OpenclDeviceInfo> enumerate_opencl_devices();

// Choose a GPU by its displayed index or by a case-insensitive part of its name.
// With neither selector, choose the first available GPU. CPU devices are never used
// as a fallback, because Phase 6 is specifically validating the GPU path.
[[nodiscard]] OpenclDeviceInfo select_opencl_gpu(std::optional<std::uint32_t> gpu_index,
                                                  std::optional<std::string_view> gpu_name);

// Format one selected device for the command-line program.
[[nodiscard]] std::string format_opencl_device(const OpenclDeviceInfo& device);

// Return a report describing the available OpenCL devices, including device type,
// global memory, compute units, and the selectable index of every GPU.
// The function can also report that OpenCL is unavailable.
[[nodiscard]] std::string opencl_device_report();

}  // namespace market_engine::app
