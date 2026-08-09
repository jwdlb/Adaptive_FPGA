# Adaptive FPGA–GPU Market Signal Engine

## 1. Delivery plan

### Goal

Build a reproducible research prototype in which:

- C++ coordinates decoded market events and all components through the normal
  live streaming path.
- A cycle-accurate Verilated SystemVerilog model owns the order book, feature calculation, and BUY/SELL/HOLD decision.
- An OpenCL GPU learner, selected independently of the surrounding runtime,
  generates bounded replacement values for the FPGA's eight weights (weight 7
  is the bias/intercept) and its BUY/SELL thresholds from recent feature history.
  The first learner may be ordinary regression; a CNN is an optional future
  implementation of the same interface.
- Complete generated parameter sets are committed atomically back into the RTL model.
- A browser dashboard observes runtime state without entering the critical processing path.

This is a heterogeneous-computing demonstration, not a live trading or execution system.

### Version 1 success criteria

The version is complete when one command can:

1. load deterministic single-instrument market events;
2. process them through a 10×10-level Verilated order book;
3. produce fixed-point features and BUY/SELL/HOLD signals;
4. prove RTL and C++ model agreement;
5. form delayed labels and train an OpenCL model asynchronously;
6. commit complete parameter sets atomically to the RTL model;
7. publish throttled state to a browser dashboard;
8. write reproducible benchmark results; and
9. pass all automated tests from a clean build.

### Scope boundaries

Version 1 includes one instrument, aggregated price levels, four event types,
eight features, a low-latency linear FPGA signal model, a pluggable OpenCL learner
with ordinary regression as the initial implementation, and simulation-only FPGA
execution.

It excludes exchange-native protocols, order-ID/FIFO matching, live data,
execution, multiple instruments, physical FPGA deployment, direct FPGA–GPU DMA,
and FPGA implementation of GPU-model inference.

## 2. Architecture and component ownership

```text
Stream 1 — market events into RTL:

CSV -> vector<MarketEvent> -> dedicated RTL worker -> RTL valid/ready input
                                                        |
                                                        v
                                      10x10 book -> features -> score/signal

Stream 2 — RTL feature results into GPU:

RTL pipeline -> one stable valid/ready result register -> dedicated RTL worker
                                                           |
                                                           v
                                       1,024-entry host SPSC result ring
                                                   |
                                                   v
                                      separate GPU worker
                                                   |
                                                   v
                            configurable OpenCL [N][8] buffer -> GpuModel

Stream 3 — GPU model updates back into RTL:

GpuModel -> newest ModelUpdate mailbox -> dedicated RTL worker
                                                   |
                                                   v
                                   RTL shadow bank + atomic commit

C++ immutable snapshots -> WebSocket server -> browser
```

Ownership rules:

- RTL is authoritative for simulated market state and signals.
- The C++ reference model is the correctness oracle during development and testing.
- C++ owns the immutable input-event vector, the lock-free single-producer/
  single-consumer result ring, configurable GPU-batch construction, delayed-label
  construction, configuration, metrics, error handling, and the safe hand-off
  between GPU and RTL.
- `LiveCoordinator` and its dedicated RTL worker coordinate the GPU and RTL. The GPU
  and Verilog do not communicate directly. Replay/reference comparison is test-only.
- OpenCL owns batched training/inference for a replaceable learner and produces a
  complete bounded `ModelUpdate`; the FPGA never waits for it.
- The dashboard receives snapshots only; it cannot modify the processing pipeline.

### Target source layout

The following layout makes the coordinator and component boundaries explicit. Existing
code is moved into these roles without changing its verified market semantics.

```text
Adaptive_FPGA/
├── src/
│   ├── main.cpp
│   ├── app/
│   │   ├── config.cpp
│   │   ├── live_coordinator.cpp
│   │   └── opencl_devices.cpp
│   ├── io/
│   │   └── event_reader.cpp
│   ├── reference/
│   │   └── reference_model.cpp
│   ├── verilator/
│   │   ├── verilator_runner.cpp
│   │   └── verilator_worker.cpp
│   ├── gpu/
│   │   ├── gpu_model.cpp
│   │   ├── gpu_worker.cpp
│   │   ├── feature_uploader.cpp
│   │   └── model_update_mailbox.cpp
│   └── market/
│       ├── event.cpp
│       └── order_book.cpp
├── include/
│   ├── app/
│   │   ├── config.hpp
│   │   ├── spsc_ring_buffer.hpp
│   │   └── live_coordinator.hpp
│   ├── io/
│   │   └── event_reader.hpp
│   ├── reference/
│   │   └── reference_model.hpp
│   ├── verilator/
│   │   ├── verilator_runner.hpp
│   │   ├── verilator_worker.hpp
│   │   └── rtl_stream.hpp
│   ├── gpu/
│   │   ├── gpu_model.hpp
│   │   ├── gpu_worker.hpp
│   │   ├── feature_uploader.hpp
│   │   └── model_update_mailbox.hpp
│   └── market/
│       ├── event.hpp
│       ├── fixed_point.hpp
│       └── order_book.hpp
├── rtl/           synthesizable hardware, including the one-result
│                  valid/ready market_stream_adapter.sv
├── tb/            Verilog-only test benches
├── tests/
│   ├── cpp/       protocol, book, reference-model, and Verilator-runner tests
│   └── fixtures/
├── config/
├── docs/
├── scripts/
└── python/
```

`main.cpp` starts the normal live path. `EventReader` loads events. `ReferenceModel`
is the test-only C++ answer key. `VerilatorWorker` exclusively owns
`VerilatorRunner` and clocks the simulated RTL. `GpuWorker` consumes the SPSC result
ring and fills mapped OpenCL memory. `LiveCoordinator` owns their lifetime and routes
the newest validated complete `ModelUpdate` back to the RTL shadow bank.

The dedicated RTL worker performs four jobs in one non-blocking stepping loop: offer
the next event from the immutable vector when `in_ready` is high, advance the
Verilated clock continuously, publish a stable RTL result directly into a reserved
host SPSC slot, and check the newest-update mailbox at safe event boundaries. It
never runs GPU kernels.

## 3. Milestones

| Milestone | Outcome | Exit gate |
|---|---|---|
| M0: Bootstrap | Buildable repository and verified dependencies | Configure/build/test works; device enumeration works |
| M1: Software oracle | Canonical protocol and deterministic C++ model | Replay is deterministic and unit-tested |
| M2: RTL order book | Parser and bounded 10-level book | 1,000,000-event differential test passes |
| M3: RTL signal path | Fixed-point features, strategy, parameter banks | Features agree within tolerance; actions agree exactly |
| M4: Runtime | Full replay through Verilator | One-command replay completes and reports metrics |
| M5: GPU/RTL contract | Safe asynchronous batches and atomic update hand-off | Contract and buffer tests pass with no ownership violation |
| M6: Adaptive loop | A pluggable GPU learner, labels, and RTL commits | Chosen CPU/GPU model agrees; model version and score update correctly |
| M7: Observability | WebSocket dashboard | Disconnect-safe 10 Hz updates with measured overhead |
| M8: Release candidate | Tests, benchmarks, docs, demo package | Clean-clone reproducibility and final demo pass |

Each milestone is gated. Work should not build on an unverified numerical or protocol layer.

## 4. Proposed schedule

The source document proposes four weeks. That is feasible only for focused full-time work with Verilator and a working OpenCL GPU available at the start. A safer estimate is five to six weeks including integration and debugging.

### Week 1 — Foundations and software oracle

- Complete M0 and M1.
- Freeze event, fixed-point, feature, and signal semantics in documentation.
- Generate deterministic sample and randomized data.

### Week 2 — RTL book and differential testing

- Complete M2.
- Concentrate on reset, backpressure, insertion/removal shifts, saturation, and invariants.

### Week 3 — RTL features, strategy, and runtime

- Complete M3 and M4.
- Define tolerances before comparison tests.
- Add optional FST/VCD tracing, disabled by default.

### Week 4 — OpenCL and adaptive loop

- Complete M5 and M6.
- Benchmark batch sizes 256, 512, 1,024, 2,048, and 4,096.

### Week 5 — Dashboard, benchmarking, and hardening

- Complete M7.
- Run sanitizers, long differential tests, and failure-path tests.
- Measure dashboard overhead and tracing overhead.

### Week 6 — Reproducibility and release buffer

- Complete M8.
- Validate on a clean environment, finish documentation, and resolve integration defects.

If only four weeks are available, preserve correctness and the adaptive loop; reduce dashboard polish and the breadth of benchmark analysis.

## 5. Detailed implementation plan

### Phase 0 — Repository and environment

Deliver:

- the documented directory layout;
- top-level CMake with strong warnings and optional sanitizers;
- dependency detection for Verilator, OpenCL, Boost, and JSON;
- a typed JSON configuration loader;
- minimal CLI and test runner;
- build, test, demo, and benchmark scripts; and
- CI for the CPU-only build plus an explicit capability report for optional hardware/GPU jobs.

Implementation notes:

- Treat missing required dependencies as clear configure-time or startup errors.
- Support `--list-opencl-devices`, `--no-gpu`, and live RTL execution early.
- Do not silently select a CPU OpenCL device when GPU was requested.
- Copy or locate OpenCL kernels deterministically from the build output.

Acceptance:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/market_engine_demo --list-opencl-devices
```

Current environment note: CMake 3.22.1, GCC 11.4, Python 3.10, Boost, and OpenCL libraries were detected. Verilator was not found on `PATH` and must be installed before RTL phases can pass.

### Phase 1 — Canonical protocol and C++ reference

Implement:

- strongly typed `MarketEvent`, `Side`, `EventType`, `FeatureVector`, `Signal`, and parameter types;
- CSV and fixed-layout binary codecs with version/magic validation;
- a 10-level bid/ask book with deterministic add, update, cancel, and trade semantics;
- Q16.16 conversion, rounding, multiplication, and saturation helpers;
- the eight-feature reference engine;
- the reference score and threshold logic;
- deterministic Python event generation; and
- invariant checks after every reference-model event.

Design decisions to freeze before RTL:

- exact invalid-price representation;
- malformed and crossed-book policy;
- cancel/trade behavior when quantity exceeds visible quantity;
- zero-denominator feature values;
- window definition and reset behavior for flow and volatility;
- signed rounding rule for fixed-point division;
- accumulator width and saturation point; and
- whether an event emits a feature/signal when one book side is empty.

Acceptance:

- repeated runs with seed 42 produce byte-identical input and output;
- unit tests cover every operation and numerical edge case;
- invalid input fails with an actionable message; and
- a sample replay produces stable book, feature, and signal snapshots.

### Phase 2 — RTL event path and order book

Implement:

- `market_types_pkg.sv` and `fixed_point_pkg.sv`;
- valid/ready parsing with stable inputs under backpressure;
- a bounded finite-state machine for search, insert, shift, update, and remove;
- explicit output/completion signaling;
- simulation-only debug ports or accessors for all 20 levels; and
- module-level and C++ differential tests.

Recommended sequence:

1. reset and one-event handshake;
2. add at empty, best, middle, worst, and beyond depth;
3. aggregation and update;
4. partial/full cancel and trade;
5. backpressure and consecutive events;
6. long randomized differential testing.

Acceptance:

- bids remain strictly descending and asks strictly ascending;
- no valid zero quantity or duplicate price remains;
- all reset and backpressure cases pass;
- the RTL matches the C++ book after every event; and
- a deterministic 1,000,000-event run completes without divergence.

On mismatch, save the seed, event index, recent event trace, book snapshots, and optional waveform command.

### Phase 3 — RTL features, strategy, and parameters

Implement features incrementally:

1. spread;
2. top-level imbalance;
3. ten-level imbalance;
4. microprice delta;
5. order-flow imbalance;
6. trade-flow imbalance;
7. short-window volatility;
8. constant bias.

Then implement:

- widened multiply-accumulate for the eight-weight score;
- explicit rounding and saturation into Q16.16;
- BUY/SELL/HOLD threshold comparisons;
- active and shadow parameter banks; and
- an atomic commit pulse with model/update counters.

Acceptance:

- every feature has directed edge-case vectors;
- RTL values match the bit-accurate C++ reference or a documented tolerance;
- action decisions match exactly, including boundary values;
- overflow is deterministic; and
- scoring never observes a partially written parameter set.

### Phase 4 — Verilator runtime

Implement an RAII `VerilatorRunner` that:

- owns the DUT, reset, clock, and optional trace;
- honors valid/ready without timing shortcuts;
- waits with a bounded timeout for event completion;
- queues feature outputs and records the latest signal;
- provides test-only book snapshots; and
- records cycle and wall-clock metrics.

Integrate replay in this order:

1. decode event;
2. update the C++ reference;
3. drive the RTL event;
4. wait for completion;
5. compare state, features, and signal;
6. append the feature snapshot to the learning pipeline.

Acceptance:

- one command runs a complete deterministic replay;
- trace output is optional and excluded from benchmarks;
- timeout and divergence errors preserve a minimal reproduction; and
- shutdown reports event count, cycles/event, throughput, and latency.

### Phase 6 — GPU/RTL integration contract and buffering

Implement:

- the `EventReader`, `LiveCoordinator`, dedicated `VerilatorWorker`, and `GpuModel`
  ownership boundaries described below; keep the reference model test-only but
  available for differential checking;
- platform/device discovery with clear selection output;
- RAII context, command queue, program, kernel, buffer, and event wrappers;
- full compiler build-log reporting;
- a versioned GPU input contract: one valid RTL feature snapshot is eight Q16.16
  values plus event metadata; a GPU-side C++ worker consumes snapshots and fills a
  configurable contiguous `[sample][8 features]` OpenCL training buffer;
- a versioned GPU output contract: one complete `ModelUpdate` contains exactly eight
  bounded replacement weights (weight 7 is the bias/intercept), bounded BUY and
  SELL threshold replacements, and a monotonically increasing version;
- an OpenCL-mappable training buffer whose row count `N` is configurable without
  changing the fixed eight-feature schema, plus a latest-result mailbox that
  publishes `ModelUpdate` only after GPU completion;
- clear mapped-buffer ownership: C++ fills the buffer only while it is mapped, and
  the GPU uses it only after C++ unmaps and submits it; and
- event-driven completion and reuse; and
- profiling only in benchmark mode;
- a live-path-to-RTL hand-off that validates a complete newest `ModelUpdate`, writes
  all ten fields to the shadow bank, and issues one commit only at an event boundary.

The earlier fixed 32 x 8 A/B host-buffer uploader remains useful OpenCL prototype
work, but it is not the final normal live transport. In the Level 2 path, the host
result ring absorbs the backlog and the GPU worker fills a configurable `N x 8`
mapped OpenCL buffer. A second OpenCL buffer is an optional later optimisation for
overlap, not a correctness requirement.

#### Level 2 streaming transport

The normal path uses a dedicated RTL execution thread and three explicit data
streams. For input, the complete CSV is parsed first into an immutable
`vector<MarketEvent>`; the RTL worker retains an index/current event and offers it
directly through the existing RTL valid/ready interface. The vector already is the
large input backlog, so the normal CSV path has neither a duplicate host event queue
nor an RTL input FIFO.

For output, the RTL has one stable compact-result register with a valid/ready
handshake. This is the minimum storage needed to prevent a one-cycle result pulse
from being lost; it is not a queue. The RTL worker reserves the next writable slot
in a fixed-capacity host SPSC ring (default 1,024 entries), decodes the RTL output
ports directly into that slot, completes the handshake, then publishes the slot by
advancing the atomic write position. There is no intermediate `RtlStreamResult`
copy and no multi-entry RTL output FIFO.
There is one producer—the RTL worker—and one consumer—the GPU-side C++ worker—so
the fast result path needs no global mutex or lock/unlock operation for every event.
Atomic read/write positions publish complete slots without producer/consumer data
corruption.

The host result ring is the only multi-entry result queue and deliberately holds the
entire output backlog. When it becomes full, the RTL worker leaves the one-result
register unaccepted; ready/valid flow control safely prevents another event result
from overwriting it and pauses new RTL work. Nothing is dropped, and ten seconds
without progress is a hard timeout.

The GPU-side C++ worker removes results from the ring, ignores invalid feature rows,
and copies each valid eight-value row directly into mapped OpenCL input memory until
`N` rows are present. It then unmaps and submits that `N x 8` buffer. It never holds
or blocks the RTL ring while OpenCL copies or trains. A mutex plus one optional
`ModelUpdate` forms a newest-value mailbox. The RTL worker checks it at a safe event
boundary and uses the existing shadow bank/atomic commit path.

Thus normal live execution has only one multi-entry queue: the 1,024-entry host SPSC
result ring. The single RTL output register is necessary handshake storage, not a
queue. The `ModelUpdateMailbox` is a separate newest-value hand-off, not a FIFO. A
host event queue is deliberately outside this project's current preloaded-CSV scope.

Acceptance:

- a known sample kernel executes on the chosen device;
- transfer ordering and buffer state tests pass;
- an in-flight buffer cannot be overwritten; and
- a test update traverses GPU result mailbox -> live coordinator -> RTL shadow bank ->
  atomic commit without a mixed parameter set; and
- pageable/pinned and synchronous/asynchronous transfer benchmarks are recorded.

### Phase 7 — Pluggable GPU learner and parameter synchronization

Implement:

- pending labelled samples containing one eight-feature row, its reference midpoint,
  and target event index; the number of rows per training batch is configurable;
- delayed binary labels based on future midpoint movement;
- a CPU floating-point oracle for the selected learner;
- OpenCL kernels for the selected learner, initially ordinary linear or logistic
  regression unless another learner is explicitly chosen;
- a model-independent `GpuModel` interface: it consumes the Phase 5 input contract
  and returns only the Phase 6 `ModelUpdate` contract;
- device-resident learner parameters and compact loss/accuracy metrics;
- configurable learning rate, L2 penalty, horizon, batch size, update cadence, and
  seed; and
- generation/readback of one complete `ModelUpdate` after each completed GPU update.

A larger learner (for example a temporal CNN) is a later interchangeable implementation:
it must use the same input batch and `ModelUpdate` output contract, so it does not
change the RTL or coordinator hand-off.

Commit sequence:

1. run inference for the newest complete sequence and enqueue asynchronous readback
   of one `ModelUpdate` packet;
2. wait only when the scheduled readback must be consumed;
3. reject non-finite values;
4. clamp the generated replacement weights and thresholds to their documented range, then convert and
   saturate them to Q16.16;
5. write the eight weight fields, BUY threshold, and SELL threshold to the RTL
   shadow bank;
6. pulse commit once;
7. verify the version increment and record latency.

Acceptance:

- CPU and GPU outputs, losses, and one-batch updates for the selected learner agree
  within a stated tolerance;
- sequences and labels preserve event order, omit invalid periods, and contain no future data;
- learner parameters stay on the GPU between updates;
- a valid commit changes the model version and subsequent RTL score; and
- non-finite, out-of-range, or partial updates never become active.

### Phase 8 — WebSocket dashboard

Implement:

- a Boost.Beast server on a separate thread or asynchronous event loop;
- static serving for native HTML/CSS/JavaScript;
- immutable runtime snapshots serialized with `nlohmann/json`;
- throttled publication at 10 Hz; and
- panels for book depth, features, action/score, weights, loss/accuracy, and latency/throughput.

Keep the processing thread non-blocking: it publishes the latest snapshot and never waits for a browser.

Acceptance:

- connect/reconnect/disconnect does not interrupt replay;
- slow clients cannot create unbounded queues;
- payload schema is documented and tested; and
- benchmark results quantify dashboard-on versus dashboard-off overhead.

### Phase 9 — Tests, benchmarks, documentation, and demo

Automated suites:

- C++ unit tests for protocol, book, fixed-point, features, strategy, labels, and configuration;
- independent RTL module tests;
- long randomized RTL/C++ differential tests;
- OpenCL CPU/GPU equivalence and buffer-order tests;
- integration tests for every boundary between components; and
- a fixed-length end-to-end smoke test.

Benchmark output:

- write raw CSV with environment metadata, configuration, seed, and commit hash;
- sweep batch sizes 256–4,096;
- measure FPGA cycles/event, host throughput, transfer latency/bandwidth, kernel time, readback time, commit latency, and dashboard overhead; and
- generate plots without replacing raw data.

Documentation:

- architecture and ownership;
- canonical protocol and numerical semantics;
- order-book operations and invariants;
- OpenCL scheduling and buffer states;
- dashboard schema;
- build/troubleshooting guide;
- limitations and honest interpretation of results; and
- clean-clone demo instructions.

Final acceptance:

```bash
./scripts/run_tests.sh
./scripts/run_demo.sh
./scripts/run_benchmark.sh
```

All three commands must be reproducible and fail loudly when a required capability is unavailable.

## 6. Testing gates

| Gate | Minimum evidence |
|---|---|
| Protocol | Round-trip CSV/binary tests and malformed-input tests |
| Reference model | Directed cases plus deterministic randomized tests |
| RTL book | Per-event comparison over 1,000,000 events |
| RTL math | Directed fixed-point boundaries and differential vectors |
| Signal | Exact decisions at, below, and above thresholds |
| OpenCL | CPU/GPU selected-model output, gradient, and one-update comparison |
| Buffers | State-machine tests under delayed completion |
| Model commit | Atomicity, versioning, finite-value rejection |
| End-to-end | Fixed replay reaches expected final checksum and metrics |

CI should run quick tests on every change. Long differential and benchmark jobs can run nightly or before release.

## 7. Major risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| C++ and RTL semantics drift | Invalid differential results | Freeze a shared protocol/numerics document before RTL |
| Fixed-point division/overflow errors | Incorrect features or signals | Widen intermediates, define rounding, test saturation boundaries |
| RTL FSM complexity | Long debugging cycle | Build one operation at a time with state snapshots |
| OpenCL platform variation | Build/runtime failures | Enumerate capabilities, print build logs, avoid vendor extensions initially |
| GPU overhead dominates the small learner | Misleading performance claims | Benchmark honestly, report asynchronous end-to-end latency, and compare against the CPU regression baseline |
| Async buffer race | Corrupt training samples | Explicit ownership states and event-dependent reuse |
| Future-label leakage/order bugs | Invalid learning results | Event-indexed pending queue with deterministic oracle tests |
| Dashboard perturbs throughput | Distorted benchmarks | Snapshot/throttle off-path and measure enabled/disabled |
| Four-week schedule pressure | Integration shortcuts | Enforce milestone gates; reduce UI polish before correctness |

## 8. Definition of done

A phase is done only when:

- implementation contains no silent placeholder behavior;
- relevant unit and integration tests pass;
- errors are explicit and actionable;
- public interfaces and numerical behavior are documented;
- build warnings introduced by the phase are resolved; and
- the phase's acceptance command works from the documented environment.

The project is done only when the end-to-end demo, full test suite, and benchmark script run from a clean checkout and the results clearly distinguish measured behavior from financial performance.

## 9. Immediate next actions

1. Install or expose Verilator and confirm the OpenCL platform sees the intended GPU.
2. Create the Phase 0 repository skeleton and dependency report.
3. Freeze the seven numerical/behavioral decisions listed in Phase 1.
4. Implement the event protocol, Q16.16 helpers, and C++ book before starting RTL.
5. Establish a deterministic final-state checksum so all later layers share one regression oracle.
