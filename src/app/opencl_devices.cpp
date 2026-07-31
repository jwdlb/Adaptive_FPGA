#include "app/opencl_devices.hpp"

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
#endif

}  // namespace

std::string opencl_device_report() {
#if !MARKET_ENGINE_HAS_OPENCL
    return "OpenCL support was not found when this project was configured.\n";
#else
    cl_uint platform_count = 0;
    const cl_int platform_status = clGetPlatformIDs(0, nullptr, &platform_count);
    if (platform_status == CL_PLATFORM_NOT_FOUND_KHR || platform_count == 0U) {
        return "No OpenCL platforms were found. Install an OpenCL ICD and GPU driver.\n";
    }
    if (platform_status != CL_SUCCESS) {
        return "Unable to enumerate OpenCL platforms (error " + std::to_string(platform_status) + ").\n";
    }

    std::vector<cl_platform_id> platforms(platform_count);
    if (clGetPlatformIDs(platform_count, platforms.data(), nullptr) != CL_SUCCESS) {
        return "Unable to retrieve OpenCL platform IDs.\n";
    }

    std::ostringstream output;
    for (cl_uint platform_index = 0; platform_index < platform_count; ++platform_index) {
        const cl_platform_id platform = platforms[platform_index];
        output << "Platform " << platform_index << ": " << platform_info(platform, CL_PLATFORM_NAME)
               << " (" << platform_info(platform, CL_PLATFORM_VENDOR) << ")\n";

        cl_uint device_count = 0;
        const cl_int device_status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &device_count);
        if (device_status == CL_DEVICE_NOT_FOUND || device_count == 0U) {
            output << "  No devices found.\n";
            continue;
        }
        if (device_status != CL_SUCCESS) {
            output << "  Unable to enumerate devices (error " << device_status << ").\n";
            continue;
        }
        std::vector<cl_device_id> devices(device_count);
        if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, device_count, devices.data(), nullptr) != CL_SUCCESS) {
            output << "  Unable to retrieve device IDs.\n";
            continue;
        }
        for (cl_uint device_index = 0; device_index < device_count; ++device_index) {
            output << "  Device " << device_index << ": " << device_info(devices[device_index], CL_DEVICE_NAME)
                   << " [" << device_info(devices[device_index], CL_DEVICE_VERSION) << "]\n";
        }
    }
    return output.str();
#endif
}

}  // namespace market_engine::app
