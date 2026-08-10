# Development environment

The supported host is Ubuntu 22.04 or newer with CMake 3.22+, a C++20 compiler,
Boost.System and Verilator. OpenCL headers/loader enable GPU compilation; a
vendor GPU runtime is required to execute training. Python 3 supports the
standalone event generator.

## Capability checks

```bash
cmake --version
c++ --version
python3 --version
verilator --version
ldconfig -p | rg 'libOpenCL|libboost_system'
```

Configure the project to see the definitive dependency report:

```bash
cmake -S . -B build
```

`market_engine_demo --list-opencl-devices` reports OpenCL platforms and devices.
If no OpenCL platform or GPU is installed, it returns a clear error. GPU support
is optional for RTL-only replay and mandatory only when explicitly running GPU
selection, smoke testing, or learning.

## Repository map

```text
include/       public C++ interfaces
src/           runtime, IO, market, GPU and Verilator implementations
rtl/           synthesizable-oriented SystemVerilog pipeline
gpu/kernels/   OpenCL health-check and training kernels
tb/            standalone RTL and differential testbenches
tests/         Catch2 integration/unit tests and fixtures
config/        runtime JSON configuration
python/        deterministic CSV generator
scripts/       build, test, demo and benchmark entry points
docs/          architecture, operations and protocol documentation
```

## Common development commands

```bash
./scripts/build.sh
ctest --test-dir build --output-on-failure
cmake --build build --target rtl_tests
cmake --build build --target rtl_differential_test
cmake --build build --target rtl_stream_adapter_tests
```

Use `-DMARKET_ENGINE_ENABLE_SANITIZERS=ON` for AddressSanitizer and UBSan
builds. Warnings are errors by default; disable that policy only for diagnosing
external toolchain incompatibilities with
`-DMARKET_ENGINE_ENABLE_WERROR=OFF`.
