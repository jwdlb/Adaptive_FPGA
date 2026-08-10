# Running the system

## Build

```bash
./scripts/build.sh
```

Equivalent manual commands:

```bash
cmake -S . -B build
cmake --build build -j
```

CMake pins and fetches nlohmann/json and Catch2. Verilator enables the live RTL
runner and RTL-specific targets. OpenCL is optional at build time; GPU commands
require a selectable GPU at runtime.

## Generate deterministic data

From the command line:

```bash
python3 python/generate_events.py \
  --output data/events.csv \
  --events 100000 \
  --seed 42
```

Or start `market_engine_demo` without `--input` and use **Generate CSV** in the
dashboard. Both generators maintain a valid local book and are deterministic
for a fixed seed.

## Run RTL replay

```bash
./build/market_engine_demo --input data/events.csv
```

Useful variants:

```bash
# Process only a prefix
./build/market_engine_demo --input data/events.csv --events 10000

# Use an alternate configuration
./build/market_engine_demo --config config/default.json --input data/events.csv

# Remove the observation server from the runtime
./build/market_engine_demo --input data/events.csv --no-dashboard
```

The executable prints configuration, mode, final throughput/cycle metrics,
checksum, signal and book. A replay stops on the first RTL book error.

## Run adaptive GPU training

Inspect and verify the OpenCL environment first:

```bash
./build/market_engine_demo --list-opencl-devices
./build/market_engine_demo --gpu-smoke-test
```

Then run:

```bash
./build/market_engine_demo \
  --input data/events.csv \
  --gpu-feature-upload \
  --batch-size 1024
```

Select a specific device by displayed GPU index or a case-insensitive name
substring:

```bash
./build/market_engine_demo --input data/events.csv --gpu-feature-upload --gpu-index 0
./build/market_engine_demo --input data/events.csv --gpu-feature-upload --gpu-name "RTX 4060"
```

Do not combine `--gpu-index` and `--gpu-name`. `--no-gpu` is a validation flag
that cannot be combined with any GPU selection or operation.

## Persist models

Model JSON is versioned and written atomically:

```bash
# Load a starting model and save the final active model
./build/market_engine_demo \
  --input data/events.csv \
  --gpu-feature-upload \
  --model-in data/model.json \
  --model-out data/model-next.json

# Checkpoint every model adopted by RTL
./build/market_engine_demo \
  --input data/events.csv \
  --gpu-feature-upload \
  --model-autosave data/model-checkpoint.json
```

`--reset-model` ignores `--model-in` and starts with zero weights plus the
configured thresholds. The initial model is committed before the first event.

## Dashboard modes

Persistent control server:

```bash
./build/market_engine_demo
```

Finite observational server:

```bash
./build/market_engine_demo --input data/events.csv
```

The latter may finish too quickly to inspect on small inputs. Use control mode
for interactive work. See [dashboard.md](dashboard.md) for endpoints and the
security boundary.

## Tests

```bash
./scripts/run_tests.sh
```

For focused invocations see [test_suite.md](test_suite.md).

## Benchmark dashboard overhead

```bash
./scripts/benchmark_dashboard.sh build
```

This compares runs with and without the observation path. Repeat measurements
on an otherwise idle host and report the environment alongside the result.

## Troubleshooting

### `--live requires a build with Verilator available`

Install Verilator, then reconfigure and rebuild. CMake decides whether to add
the operational runner during configuration.

### No OpenCL platform or selectable GPU

The loader alone is insufficient; install the GPU vendor's OpenCL runtime.
Use `--list-opencl-devices` to distinguish a missing platform from a platform
that exposes only CPU devices.

### GPU run produces no batches

The stream needs at least one complete batch of valid, labelable rows. Supply
more than `labelHorizonEvents + featureBatchSize` events and ensure both sides
of the book remain populated. The incomplete final batch is discarded.

### RTL result ring timeout

The consumer did not release ring capacity before the bounded timeout. Check
for a stalled/very slow OpenCL batch, GPU driver errors, extreme batch sizes,
or instrumentation that starves the GPU worker.

### Dashboard is unreachable

Confirm the printed bind address and port, then query:

```bash
curl http://127.0.0.1:8080/health
```

Another process may already own the configured port. The server reports bind
failures and exits rather than silently selecting a different port.

### Generated input is rejected

Confirm the exact CSV header and event rules in [protocol.md](protocol.md).
Parsing validates the full file before the RTL worker starts.

