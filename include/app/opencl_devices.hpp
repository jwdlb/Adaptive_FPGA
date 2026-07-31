#pragma once  // Prevent duplicate inclusion.

#include <string>

namespace market_engine::app {

// Return a report describing the available OpenCL devices.
// The function can also report that OpenCL is unavailable.
[[nodiscard]] std::string opencl_device_report();

}  // namespace market_engine::app
