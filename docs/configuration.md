# Configuration reference

The application loads `config/default.json` unless `--config PATH` is given.
Command-line overrides are applied after the file is parsed. Unknown CLI
options and invalid values are fatal configuration errors.

## JSON fields

| Field | Default | Runtime effect |
| --- | ---: | --- |
| `orderBookDepth` | `10` | Validated as exactly 10; matches the compiled RTL depth |
| `clockPeriodNs` | `10` | Verilator simulated clock period |
| `featureWindowEvents` | `64` | Accepted/validated, but the RTL window is currently compiled at 64 and is not parameterized from JSON |
| `featureBatchSize` | `1024` | Number of labelled rows in a GPU batch |
| `learningRate` | `0.001` | Q16.16 learning rate passed to the GPU batch update |
| `l2Regularisation` | `0.0001` | Q16.16 L2 coefficient applied to each weight |
| `labelHorizonEvents` | `100` | Minimum event-index distance between a feature row and its future label |
| `weightUpdateIntervalBatches` | `10` | Accepted/validated but currently unused; every completed full batch publishes an update |
| `buyThreshold` | `0.2` | Initial BUY threshold; must exceed the SELL threshold |
| `sellThreshold` | `-0.2` | Initial SELL threshold |
| `dashboardPort` | `8080` | HTTP/WebSocket listening port |
| `dashboardUpdateHz` | `10` | Snapshot publish and per-client send frequency |
| `dashboardBindAddress` | `127.0.0.1` | Numeric local bind address |
| `enableTrace` | `false` | Accepted and printable, but waveform trace generation is not implemented |
| `enableOpenCLProfiling` | `false` | Accepted but currently unused; the OpenCL queue always enables profiling for reported timings |
| `allowCrossedBooks` | `false` | Used by the C++ reference model; operational RTL is compiled with crossed books disabled |
| `randomSeed` | `42` | Default seed for dashboard-generated CSV data |

The table distinguishes configured behavior from fields retained for planned
integration. This prevents configuration that parses successfully from being
mistaken for an active runtime control.

## Validation

- depth must be 10;
- clock, window, batch, horizon, update interval and dashboard rate must be
  positive;
- learning rate must be finite and positive;
- L2 regularisation must be finite and non-negative;
- BUY and SELL thresholds must be finite, with BUY strictly greater than SELL;
- bind address must not be empty.

## Command-line reference

Run `./build/market_engine_demo --help` for the authoritative list.

| Option | Meaning |
| --- | --- |
| `--config PATH` | Load alternate JSON configuration |
| `--input PATH` | Replay CSV or MKT1 input; omission starts dashboard control mode |
| `--events N` | Process only the first N events |
| `--seed N` | Override generator seed used by dashboard commands |
| `--batch-size N` | Override `featureBatchSize` |
| `--gpu-feature-upload` | Enable the online GPU worker/training loop |
| `--gpu-index N` | Select or inspect a GPU by reported index |
| `--gpu-name TEXT` | Select or inspect by device-name substring |
| `--list-opencl-devices` | Print all discovered OpenCL devices |
| `--select-gpu` | Select and print the requested/default GPU, then exit |
| `--gpu-smoke-test` | Run the OpenCL `[1,2,3] → [2,4,6]` health check |
| `--no-gpu` | Assert a GPU-disabled invocation; incompatible with GPU options |
| `--model-in PATH` | Load a versioned initial model |
| `--model-out PATH` | Atomically save the final active model |
| `--model-autosave PATH` | Atomically save each model adopted by RTL |
| `--reset-model` | Ignore `--model-in` and use zero weights/configured thresholds |
| `--no-dashboard` | Create neither dashboard server nor snapshot publisher |
| `--trace` | Set the currently reserved trace flag |
| `--benchmark` | Compatibility flag; use the benchmark script |
| `--live` | Explicit spelling of the default RTL processing mode |

## Fixed versus configurable dimensions

The following are compile-time protocol properties in version 1:

- ten book levels per side;
- eight model features;
- 64 entries in the RTL feature window;
- ten model parameter words (eight weights and two thresholds);
- 1,024 entries in the host result ring;
- one-tick minimum label profit in `LiveCoordinator`.

Changing these values requires coordinated C++, SystemVerilog, OpenCL kernel,
test and protocol updates; editing JSON alone is insufficient.

