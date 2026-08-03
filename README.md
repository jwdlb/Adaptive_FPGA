# Adaptive FPGA–GPU Market Signal Engine

A correctness-first research prototype for market-event replay, a synthesizable
SystemVerilog market pipeline simulated with Verilator, batched OpenCL online
learning, and browser observability.

## Current status

The C++ reference path is implemented: protocol codecs, fixed-point arithmetic,
a ten-level order book, feature calculation, strategy evaluation, replay
metrics, deterministic fixtures, and a seeded event generator. RTL, GPU
learning, and the dashboard remain planned.

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

Generate a deterministic stream and replay it through the C++ reference:

```bash
python3 python/generate_events.py --output data/events.csv --seed 42 --events 1000000
./build/market_engine_demo --reference-only --input data/events.csv
```

Use `--events N` to replay only the first `N` records. On an order-book replay
failure, the demo writes `failure_repro.csv` containing the failing event and
up to 100 preceding events. RTL/GPU processing is not implemented yet.
