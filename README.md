# Adaptive FPGA–GPU Market Signal Engine

A correctness-first research prototype with a normal event-processing path, a
synthesizable SystemVerilog market pipeline simulated with Verilator, GPU
feature uploads through OpenCL, and separate C++/RTL test support.

## Current status

The normal application processes events through simulated RTL and can upload
valid 32 x 8 feature batches to a selected GPU. The C++ reference model and
replay comparison harness are test-only. GPU learning/model updates and the
dashboard remain planned.

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
./build/market_engine_demo --select-gpu
./build/market_engine_demo --gpu-index 0
./build/market_engine_demo --gpu-smoke-test
```

Generate a deterministic stream and process it through the normal RTL path:

```bash
python3 python/generate_events.py --output data/events.csv --seed 42 --events 1000000
./build/market_engine_demo --input data/events.csv
```

Use `--events N` to process only the first `N` records. Add
`--gpu-feature-upload` on a machine with a selected OpenCL GPU to send valid
feature batches to it. `ctest --test-dir build` runs the separate C++/RTL
comparison replay tests.
