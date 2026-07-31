# Development environment

The supported Phase 0 host is Ubuntu 22.04 or newer with CMake 3.22+, a C++20
compiler, Boost.System, OpenCL headers/loader, and Python 3. Verilator is
required before the RTL phases begin.

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
is optional during Phase 0; it becomes mandatory only when explicitly running
the later GPU learning mode.

