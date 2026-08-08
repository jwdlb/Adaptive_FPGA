// OpenCL device discovery and GPU selection for Adaptive_FPGA.
//
// This file finds the OpenCL platforms and devices available on the host,
// describes them for command-line output, and selects one GPU for the
// project's GPU compute path. It does not create an OpenCL context or run
// kernels; later code uses the selected device information to do that.
#include "app/opencl_devices.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

#if MARKET_ENGINE_HAS_OPENCL
#include <CL/cl.h>
#include <CL/cl_ext.h>
#endif

namespace market_engine::app {
namespace {

#if MARKET_ENGINE_HAS_OPENCL
struct DiscoveryResult {
    bool platform_found{false};
    std::vector<OpenclDeviceInfo> devices{};
};

// Read one variable-length text field from an OpenCL platform.
[[nodiscard]] std::string platform_info(cl_platform_id platform, cl_platform_info field) {
    std::size_t size = 0;
    if (clGetPlatformInfo(platform, field, 0, nullptr, &size) != CL_SUCCESS) {
        return "<unavailable>";
    }
    std::string value(size, '\0');
    if (clGetPlatformInfo(platform, field, size, value.data(), nullptr) != CL_SUCCESS) {
        return "<unavailable>";
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

// Read one variable-length text field from an OpenCL device.
[[nodiscard]] std::string device_info(cl_device_id device, cl_device_info field) {
    std::size_t size = 0;
    if (clGetDeviceInfo(device, field, 0, nullptr, &size) != CL_SUCCESS) {
        return "<unavailable>";
    }
    std::string value(size, '\0');
    if (clGetDeviceInfo(device, field, size, value.data(), nullptr) != CL_SUCCESS) {
        return "<unavailable>";
    }
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

// Read one fixed-size numeric field from an OpenCL device, or return zero on failure.
template <typename T>
[[nodiscard]] T device_value(cl_device_id device, cl_device_info field) {
    T value{};
    return clGetDeviceInfo(device, field, sizeof(value), &value, nullptr) == CL_SUCCESS ? value : T{};
}

// Turn OpenCL device-type flags into a readable comma-separated name.
[[nodiscard]] std::string device_type_name(cl_device_type type) {
    std::vector<std::string> names;
    if ((type & CL_DEVICE_TYPE_GPU) != 0U) names.emplace_back("GPU");
    if ((type & CL_DEVICE_TYPE_CPU) != 0U) names.emplace_back("CPU");
    if ((type & CL_DEVICE_TYPE_ACCELERATOR) != 0U) names.emplace_back("accelerator");
    if ((type & CL_DEVICE_TYPE_CUSTOM) != 0U) names.emplace_back("custom");
    if (names.empty()) return "other";

    std::ostringstream output;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index != 0U) output << ", ";
        output << names[index];
    }
    return output.str();
}

// Find every OpenCL platform and device exposed by the host machine.
// GPUs receive a project-wide selection index; other device types are reported
// but are not assigned a selectable GPU index.
[[nodiscard]] DiscoveryResult discover_opencl_devices() {
    // Ask OpenCL how many platforms exist before allocating their ID list.
    cl_uint platform_count = 0;
    const cl_int platform_status = clGetPlatformIDs(0, nullptr, &platform_count);
    // A missing OpenCL driver/runtime is a normal discovery result, not an error.
    if (platform_status == CL_PLATFORM_NOT_FOUND_KHR || platform_count == 0U) {
        return {};
    }
    if (platform_status != CL_SUCCESS) {
        throw std::runtime_error("unable to enumerate OpenCL platforms (error " + std::to_string(platform_status) + ")");
    }

    // Retrieve the platform IDs now that their count is known.
    std::vector<cl_platform_id> platforms(platform_count);
    if (clGetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
        throw std::runtime_error("unable to retrieve OpenCL platform IDs");
    }

    DiscoveryResult result;
    result.platform_found = true;
    // GPU indices are numbered across all platforms so --gpu-index is unambiguous.
    std::uint32_t next_gpu_index = 0;
    for (cl_uint platform_index = 0; platform_index < platform_count; ++platform_index) {
        const cl_platform_id platform = platforms[platform_index];
        // Count all device types on this platform before retrieving their IDs.
        cl_uint device_count = 0;
        const cl_int device_status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
        if (device_status == CL_DEVICE_NOT_FOUND || device_count == 0U) continue;
        if (device_status != CL_SUCCESS) {
            throw std::runtime_error("unable to enumerate OpenCL devices (error " + std::to_string(device_status) + ")");
        }

        // Retrieve each device, then collect the descriptive fields used by the CLI.
        std::vector<cl_device_id> devices(device_count);
        if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr) != CL_SUCCESS) {
            throw std::runtime_error("unable to retrieve OpenCL device IDs");
        }
        for (cl_uint device_index = 0; device_index < device_count; ++device_index) {
            const cl_device_id device = devices[device_index];
            const cl_device_type type = device_value<cl_device_type>(device, CL_DEVICE_TYPE);
            OpenclDeviceInfo info{
                .platform_index = platform_index,
                .device_index = device_index,
                .platform_name = platform_info(platform, CL_PLATFORM_NAME),
                .platform_vendor = platform_info(platform, CL_PLATFORM_VENDOR),
                .platform_version = platform_info(platform, CL_PLATFORM_VERSION),
                .name = device_info(device, CL_DEVICE_NAME),
                .vendor = device_info(device, CL_DEVICE_VENDOR),
                .version = device_info(device, CL_DEVICE_VERSION),
                .type = device_type_name(type),
                .global_memory_bytes = device_value<cl_ulong>(device, CL_DEVICE_GLOBAL_MEM_SIZE),
                .max_compute_units = device_value<cl_uint>(device, CL_DEVICE_MAX_COMPUTE_UNITS),
            };
            // Keep CPUs and other devices in the report, but only GPUs can be selected.
            if ((type & CL_DEVICE_TYPE_GPU) != 0U) {
                info.gpu_index = next_gpu_index++;
            }
            result.devices.push_back(std::move(info));
        }
    }
    return result;
}

// Return a lowercase copy for case-insensitive GPU-name matching.
[[nodiscard]] std::string lower_case(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}
#endif

}  // namespace

// Return structured information about every discovered OpenCL device.
// Builds without OpenCL support return an empty list.
std::vector<OpenclDeviceInfo> enumerate_opencl_devices() {
#if !MARKET_ENGINE_HAS_OPENCL
    return {};
#else
    return discover_opencl_devices().devices;
#endif
}

// Choose one GPU by its selection index or a case-insensitive part of its name.
// With no selector, choose the first GPU; never fall back to a CPU device.
OpenclDeviceInfo select_opencl_gpu(std::optional<std::uint32_t> gpu_index,
                                   std::optional<std::string_view> gpu_name) {
#if !MARKET_ENGINE_HAS_OPENCL
    throw OpenclSelectionError("OpenCL support was not found when this project was configured");
#else
    const DiscoveryResult discovery = discover_opencl_devices();
    // Give a more useful error when the host has no OpenCL runtime at all.
    if (!discovery.platform_found) {
        throw OpenclSelectionError("no OpenCL platforms were found; install an OpenCL ICD and GPU driver");
    }

    // Filter the full report down to the device type this project will use.
    std::vector<OpenclDeviceInfo> gpus;
    for (const OpenclDeviceInfo& device : discovery.devices) {
        if (device.gpu_index) gpus.push_back(device);
    }
    if (gpus.empty()) {
        throw OpenclSelectionError("no OpenCL GPU devices were found; CPU devices are not used as a fallback");
    }
    if (gpu_index) {
        // An explicit index takes priority over name matching.
        for (const OpenclDeviceInfo& gpu : gpus) {
            if (gpu.gpu_index == gpu_index) return gpu;
        }
        throw OpenclSelectionError("GPU index " + std::to_string(*gpu_index) + " was not found; use --list-opencl-devices");
    }
    // No selection option means use the first GPU reported by OpenCL.
    if (!gpu_name) return gpus.front();

    // Compare lowercase names so --gpu-name is case-insensitive and may use a substring.
    const std::string requested_name = lower_case(*gpu_name);
    std::vector<OpenclDeviceInfo> matches;
    for (const OpenclDeviceInfo& gpu : gpus) {
        if (lower_case(gpu.name).find(requested_name) != std::string::npos) matches.push_back(gpu);
    }
    if (matches.empty()) {
        throw OpenclSelectionError("no GPU name contains '" + std::string(*gpu_name) + "'; use --list-opencl-devices");
    }
    // Do not silently choose between similar GPU names; require an exact index instead.
    if (matches.size() != 1U) {
        throw OpenclSelectionError("GPU name '" + std::string(*gpu_name) + "' matches more than one GPU; use --gpu-index");
    }
    return matches.front();
#endif
}

// Format one device as a human-readable multi-line command-line description.
std::string format_opencl_device(const OpenclDeviceInfo& device) {
    std::ostringstream output;
    // Convert bytes to MiB to keep the hardware summary easy to read.
    output << "  GPU index: " << (device.gpu_index ? std::to_string(*device.gpu_index) : "not a GPU") << "\n"
           << "  Platform: " << device.platform_name << " (" << device.platform_vendor << ")\n"
           << "  Platform version: " << device.platform_version << "\n"
           << "  Device: " << device.name << " (" << device.vendor << ")\n"
           << "  Device version: " << device.version << "\n"
           << "  Type: " << device.type << "\n"
           << "  Global memory: " << device.global_memory_bytes / (1024ULL * 1024ULL) << " MiB\n"
           << "  Compute units: " << device.max_compute_units << '\n';
    return output.str();
}

// Return a report of all OpenCL platforms and devices, including helpful
// diagnostics when OpenCL is unavailable or discovery fails.
std::string opencl_device_report() {
#if !MARKET_ENGINE_HAS_OPENCL
    return "OpenCL support was not found when this project was configured.\n";
#else
    try {
        const DiscoveryResult discovery = discover_opencl_devices();
        // Distinguish no driver/runtime from a platform that simply has no devices.
        if (!discovery.platform_found) {
            return "No OpenCL platforms were found. Install an OpenCL ICD and GPU driver.\n";
        }

        std::ostringstream output;
        if (discovery.devices.empty()) {
            output << "OpenCL platforms were found, but they expose no devices.\n";
        }
        bool gpu_found = false;
        for (const OpenclDeviceInfo& device : discovery.devices) {
            // Print all devices so users can diagnose a CPU-only OpenCL setup.
            output << "Platform " << device.platform_index << ": " << device.platform_name
                   << " (" << device.platform_vendor << ")\n"
                   << "  Platform version: " << device.platform_version << "\n"
                   << "  Device " << device.device_index << ": " << device.name << "\n"
                   << "    Vendor: " << device.vendor << "\n"
                   << "    Version: " << device.version << "\n"
                   << "    Type: " << device.type << "\n"
                   << "    Global memory: " << device.global_memory_bytes / (1024ULL * 1024ULL) << " MiB\n"
                   << "    Compute units: " << device.max_compute_units << "\n";
            if (device.gpu_index) {
                gpu_found = true;
                // Show the stable index accepted by the --gpu-index command-line option.
                output << "    GPU selection index: " << *device.gpu_index << "\n";
            }
        }
        // Make the project's GPU-only policy explicit in a CPU-only environment.
        if (!discovery.devices.empty() && !gpu_found) {
            output << "No OpenCL GPU devices were found. CPU devices will not be selected as a fallback.\n";
        }
        return output.str();
    } catch (const std::exception& error) {
        return "Unable to enumerate OpenCL devices: " + std::string(error.what()) + ".\n";
    }
#endif
}

}  // namespace market_engine::app
