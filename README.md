# Adaptive FPGA–GPU Market Signal Engine

A correctness-first research prototype for market-event replay, a synthesizable
SystemVerilog market pipeline simulated with Verilator, batched OpenCL online
learning, and browser observability.

## Current status

Phase 0 is implemented: CMake, dependency/capability checks, configuration,
CLI, tests, scripts, and CI. The order book, RTL, GPU learner, and dashboard
are planned but not implemented yet.

## Prerequisites

- Ubuntu 22.04 or newer
- CMake 3.22 or newer
- C++20 compiler (GCC 11 or newer recommended)
- Boost.System development files
- OpenCL headers and loader library
- Python 3
- Verilator for future RTL phases

CMake downloads the pinned Catch2 and nlohmann/json dependencies during the
first configure.

## Build and test

```bash
./scripts/build.sh
./scripts/run_tests.sh
./build/market_engine_demo --help
./build/market_engine_demo --list-opencl-devices
```

The application currently validates and displays its effective configuration;
it does not yet replay events or run RTL/GPU processing.

