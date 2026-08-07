# Incremental Implementation Plan

This checklist converts the project design into small, reviewable changes.
Complete one numbered step, run its check, commit it, then move on. Do not
combine a later phase with an unfinished correctness gate.

## Working rules

- Each numbered step is one small implementation task and one commit.
- Keep `main` buildable and runnable in reference-only mode.
- Use deterministic seeds. On any mismatch, save the seed, event index, and
  preceding 100 events before investigating.
- The order-book, feature, strategy, and parameter-bank RTL stay synthesizable.
  Debug visibility is isolated in the Verilator wrapper or synthesis guards.
- Never add GPU/dashboard performance work before C++/RTL agreement is proven.
- Optional capability failures are explicit; requesting GPU never falls back to
  CPU without the user selecting a no-GPU/reference-only mode.
- `ReplayCoordinator` is the only GPU/RTL bridge. Verilog never calls GPU software,
  and the GPU never writes RTL state directly.
- The GPU boundary is model-independent: valid RTL snapshots enter as eight Q16.16
  features plus metadata, and every learner returns one complete nine-value
  `ModelUpdate` (eight weight adjustments and one bias adjustment) plus version and
  completion state.

## Phase 0 — Bootstrap

### 0.1 Record prerequisites

Create `docs/development.md` with Ubuntu version, compiler, CMake, Python,
Verilator, Boost, OpenCL, and GPU requirements plus installation/check commands.

Check: every command produces a clear version or capability result.

### 0.2 Create the layout and ignore rules

Add `config`, `data`, `docs`, `include`, `src`, `rtl`, `kernels`, `tests`,
`python`, `scripts`, and `web`; reserve `src/include` component pairs for `app`,
`io`, `reference`, `verilator`, `gpu`, and `market`. Ignore build output, waveforms,
generated data, and benchmark output.

Check: `git status` contains no generated files after a build.

### 0.3 Create top-level CMake

Require CMake 3.22+, C++20, and strict warnings. Put common compiler settings
in one interface target.

Check: `cmake -S . -B build` configures.

### 0.4 Add dependencies

Locate system Boost.System, OpenCL, Python, and Verilator. Pin Catch2 3.7.1
and nlohmann/json 3.11.3 through `FetchContent`. Verilator/OpenCL remain
optional for the initial reference-only target.

Check: CMake prints a dependency report naming missing capabilities.

### 0.5 Add the executable and tests

Create `market_engine_demo` with `--help` and `--version`; create one Catch2
test and register it with CTest.

Check:

```bash
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 0.6 Add scripts and CI

Add `build.sh`, `run_tests.sh`, `run_demo.sh`, and `run_benchmark.sh`; add an
Ubuntu CI job for build, C++ tests, Python tests, and a reference-only smoke run.

Check: scripts work when called outside the repository root.

### 0.7 Add JSON configuration

Add the default configuration: depth 10, 10 ns clock, 64-event feature window,
32-event GPU sequence, batch 1,024, label horizon 100, learning rate 0.001, L2
0.0001, one GPU-generated parameter update per completed batch, dashboard port
8080/rate 10 Hz, and seed 42. Model-specific settings belong to the selected
Phase 7 learner, not to the common GPU/RTL contract.

Check: demo parses and prints the effective configuration.

### 0.8 Validate configuration and CLI

Reject unsupported depth, zero batch/window, invalid port, invalid rates, and
invalid threshold ordering. Add `--config`, `--input`, `--events`, `--seed`,
`--batch-size`, `--reference-only`, `--no-gpu`, `--no-dashboard`, `--trace`,
`--benchmark`, and `--list-opencl-devices`.

Check: unit tests cover every validation failure and CLI overrides JSON.

**Gate:** clean configure/build/test works and all failures are actionable.

## Phase 1 — Protocol and numerical foundation

### 1.1 Define core types

Create strongly typed `EventType`, `Side`, `Action`, `MarketEvent`,
`FeatureVector`, `Signal`, and `ModelParameters` types.

Check: enum encodings are asserted in tests.

### 1.2 Define event validation

Prices must be positive. Add/Cancel/Trade quantities must be non-zero; Update
may be zero. Unknown enum values and malformed input are errors.

Check: valid and invalid examples exist for every event type.

### 1.3 Implement CSV codec

Read one canonical event per line with line-numbered malformed-input errors.

Check: valid, missing-field, extra-field, invalid-enum, and overflow fixtures.

### 1.4 Implement versioned binary codec

Use explicit little-endian fields with magic, format version, and record size.
Never serialize native C++ struct memory.

Check: CSV → binary → events preserves every field; fixture checksum is stable.

### 1.5 Implement Q16.16 conversion

Implement saturating integer/float conversion and round-to-nearest with ties
away from zero.

Check: positive/negative ties, minima/maxima, and saturation vectors.

### 1.6 Implement fixed-point multiply/divide

Use widened signed intermediates. Division by zero is an explicit error.

Check: hand-computed positive, negative, and boundary vectors.

### 1.7 Create shared protocol/numerics docs

Document wire fields, validity rules, widths, rounding, saturation, and action
encodings before RTL is started.

Check: every documentation example is a unit-test vector.

**Gate:** input and numerical behavior are frozen and testable in C++.

## Phase 2 — C++ order book and feature oracle

### 2.1 Create book storage

Represent each side as ten price/quantity levels with zero quantity and a
defined invalid price for unused entries.

Check: a new book is empty and passes invariants.

### 2.2 Add invariant checking

Verify strict bid-descending/ask-ascending order, no duplicates, no valid zero
quantities, and no crossed book by default.

Check: each handcrafted invariant violation produces a targeted message.

### 2.3 Implement bounded lookup

Return either a matching level or its sorted insertion position.

Check: empty, best, middle, worst, and missing-price cases for both sides.

### 2.4 Implement Add

Aggregate existing price levels; otherwise insert, shift worse levels, and drop
the worst level beyond depth. Saturate quantity at `UINT32_MAX`.

Check: all insertion positions, aggregation, saturation, and depth overflow.

### 2.5 Implement Update

Set an existing aggregate quantity; zero removes it; unknown price is invalid.

Check: non-zero, zero, and missing-price update cases.

### 2.6 Implement level removal

Compact all later levels and clear the final slot.

Check: remove best, middle, worst, and only level.

### 2.7 Implement Cancel

Subtract with saturation and remove a zero remainder; missing price is invalid.

Check: partial, exact, oversized, and missing cancellation.

### 2.8 Implement Trade

Reduce visible quantity like cancel and produce signed aggressive-flow delta:
ask trades positive, bid trades negative.

Check: both sides, partial/full reductions, and missing-price error.

### 2.9 Implement event application

Validate first, apply one operation, check invariants, and return an error code
plus book/flow deltas. Reject events that would create a crossed book.

Check: invalid or crossing event leaves snapshot/checksum unchanged.

### 2.10 Implement midpoint and microprice

Return no value if either side is empty or any denominator is zero.

Check: balanced, asymmetric, and one-sided vectors.

### 2.11 Implement the 64-event ring window

Store per-event order-flow, trade-flow, and absolute midpoint changes; update
rolling sums as entries expire.

Check: fill, rollover, reset, and exact-expiry behavior.

### 2.12 Implement features one at a time

Implement normalized spread, L1 imbalance, L10 imbalance, normalized
microprice delta, order-flow imbalance, trade-flow imbalance, normalized mean
absolute midpoint movement, and constant bias `1.0`.

Check: golden vectors for balanced, imbalanced, sparse, zero-flow, extreme,
and empty-book cases.

### 2.13 Implement reference strategy

Use widened Q16.16 score accumulation. Strictly greater/less thresholds emit
BUY/SELL; equality emits HOLD. Invalid features emit invalid HOLD.

Check: BUY, SELL, HOLD, equality, negative weights, and overflow.

### 2.14 Implement deterministic checksum and diagnostic snapshot

Hash final book, counters, feature vector, signal, and parameter version.

Check: repeated seeded replay gives the identical checksum.

**Gate:** C++ is a deterministic, bit-accurate correctness oracle.

## Phase 3 — Input data and reference demo

### 3.1 Build the deterministic Python generator

Generate valid add/update/cancel/trade streams with configurable seed and
event count, preserving a valid book.

Check: same seed gives byte-identical output.

### 3.2 Add directed fixtures

Create small CSV fixtures for every book edge case and expected final checksum.

Check: fixture test suite runs in milliseconds.

### 3.3 Add long replay generation

Generate one million events on demand instead of committing a huge data file.

Check: output reports seed, count, and version.

### 3.4 Add reference replay metrics

Report processed events, errors, events/s, final book, and checksum.

Check: `--events N` processes exactly N records.

### 3.5 Add failure reproduction artifact

On test mismatch, write seed, event index, and 100 preceding events.

Check: replaying the artifact reproduces the mismatch.

### 3.6 Freeze v1 semantics

Update protocol, book, and feature documentation from the verified fixtures.

Check: docs formulas link to golden tests.

**Gate:** the reference-only demo runs on any supported Ubuntu host.

## Phase 4 — Synthesizable RTL book

### 4.1 Add RTL type packages

Create packed event/side/action types, explicit widths, and fixed-point
constants matching the C++ definitions.

Check: Verilator lint passes packages.

### 4.2 Add top-level reset and clock behavior

Implement synchronous active-low reset with defined outputs.

Check: reset test proves known defaults.

### 4.3 Add input handshake

Latch an event only on `in_valid && in_ready`; deassert ready while processing.

Check: held-valid, bubbles, and backpressure tests.

### 4.4 Add completion/error interface

Emit exactly one `event_done` pulse and event error result per accepted event.

Check: accepted-event and completion counters always match.

### 4.5 Add one-side storage and search FSM

Store ten levels and scan at most ten cycles to find a price/insertion point.

Check: directed lookup vectors match C++.

### 4.6 Implement existing-price Add

Update a matching level with saturated addition.

Check: aggregation and saturation match C++.

### 4.7 Implement insertion FSM

Shift one level per cycle from worst toward insertion position, then write.

Check: best/middle/worst/depth-overflow insertion vectors.

### 4.8 Implement Update and removal FSM

Set an existing quantity; zero branches to compacting removal.

Check: every removal index and missing update.

### 4.9 Implement Cancel and Trade

Apply saturated reduction; expose trade delta to the feature engine.

Check: all directed C++ operation fixtures match.

### 4.10 Add crossed-book protection

Reject a state-changing operation which would cross best bid/ask, leaving all
state untouched. A test-only configuration may permit crossed-state vectors.

Check: crossing Add/Update tests.

### 4.11 Add simulation snapshot wrapper

Expose book snapshots outside the synthesis core only.

Check: the core has no simulation-only behavioral construct.

### 4.12 Add standalone RTL tests

Cover reset, valid/ready, every book operation, errors, and backpressure.

Check: `rtl_tests` runs independently from the application.

### 4.13 Implement baseline Verilator runner

Own DUT, clock, reset, ready-aware submission, bounded timeout, and snapshot
access through RAII.

Check: one directed event completes within a documented cycle limit.

### 4.14 Add full differential book test

Apply every generated event to C++ then RTL; compare all 20 levels and errors.

Check: seeded one-million-event replay has no divergence.

**Gate:** RTL book behavior is proven equivalent to C++ before features begin.

## Phase 5 — RTL features, strategy, runtime, and coordinator refactor

### 5.0 Establish the runtime component boundaries

Keep the existing market semantics unchanged, but split the application into:
`EventReader` (`io/`), optional `ReferenceModel` (`reference/`),
`VerilatorRunner` (`verilator/`), `ReplayCoordinator` (`app/`), and future
`GpuModel` (`gpu/`). Keep `main.cpp` limited to option parsing and selecting a
runtime mode.

Check: reference-only replay produces the same final checksum before and after the
move, and all existing C++ tests remain green.

### 5.1 Port fixed-point primitives to RTL

Implement widened arithmetic, rounding, division, and saturation as package
functions with no implicit-width expressions.

Check: shared numerical vectors are bit-exact.

### 5.2 Add feature-valid logic

Detect unavailable sides/denominators before calculation.

Check: invalid book yields `feature_valid=0` and invalid HOLD signal.

### 5.3 Add features incrementally

Implement spread, L1, L10, microprice delta, order flow, trade flow,
volatility, then bias—one change and test suite per feature.

Check: each feature matches C++ on directed and randomized vectors.

### 5.4 Add feature window storage

Implement synthesizable 64-entry rings for flow and volatility inputs.

Check: rollover and reset compare exactly with C++.

### 5.5 Add parameter banks

Implement active/shadow weights, thresholds, model version, update counter,
and a complete-write bitmask.

Check: incomplete shadow writes cannot commit.

### 5.6 Add atomic parameter commit

Swap banks only between completed events; increment version once.

Check: score trace uses either all old or all new parameters.

### 5.7 Add weighted score and action

Multiply eight weight/feature pairs at widened width; saturate output and apply
strict thresholds.

Check: exact decision boundaries and overflow vectors.

### 5.8 Extend Verilator runner

Queue feature snapshots, retain latest signal, expose parameter writes/commit,
and record cycles.

Check: runner captures a feature and signal for one valid event.

### 5.9 Add full differential tests

Compare book, feature validity/vector, score, action, and model version after
every event and commit.

Check: directed and randomized replay agree with the oracle.

### 5.10 Add a complete RTL demo path

Run replay, compare output, optionally write FST trace, and print metrics.

Check: one command completes with tracing disabled by default.

**Gate:** the entire simulated hardware pipeline is correct before GPU work.

## Phase 6 — GPU/RTL contract and OpenCL infrastructure

Phase 6 proves the reusable connection, not a particular learning algorithm. It must
be possible to replace the Phase 7 learner without changing RTL, `VerilatorRunner`,
or `ReplayCoordinator`.

### 6.1 Enumerate OpenCL platforms/devices

Print platform/vendor/version and device name/type/memory/compute units.

Check: no-platform and no-GPU errors are distinct and readable.

### 6.2 Add explicit GPU selection

Select by index/name or first GPU; do not silently use CPU.

Check: unavailable selection returns non-zero.

### 6.3 Create RAII OpenCL wrappers

Own context, queue, program, kernel, buffers, and events.

Check: repeated construction/destruction is sanitizer-clean.

### 6.4 Compile a smoke kernel

Load kernels from a deterministic build-installed location and print full build
log on compilation failure.

Check: vector copy GPU output matches host data.

### 6.5 Define the model-independent GPU schemas

Define, document, and test two versioned packed contracts:

- `FeatureSnapshot`: eight valid Q16.16 features, event index, timestamp, model
  version, and validity state, received from RTL through `VerilatorRunner`.
- `ModelUpdate`: exactly eight bounded Q16.16 weight adjustments, one bounded Q16.16
  bias adjustment, monotonically increasing update version, and a complete marker.

The GPU and RTL do not communicate directly. `ReplayCoordinator` owns the conversion,
validation, and hand-off between these contracts.

Check: byte layout, field order, versions, and invalid-packet rejection are covered by
host tests.

### 6.6 Define batch memory layout

Use contiguous `[sample][time][feature]` storage for 32 x 8 feature sequences,
assembled by `ReplayCoordinator` from valid `FeatureSnapshot` values. The GPU owns no
RTL window state; it receives completed feature snapshots only. Document element order,
alignment, and the absence of future data.

Check: host packing test verifies positions and ordering.

### 6.7 Allocate mapped pinned buffers and latest-result mailbox

Create two `CL_MEM_ALLOC_HOST_PTR` buffers and map once where supported.
Allocate a result mailbox capable of holding one complete `ModelUpdate`; retaining the
newest complete update is allowed, but partial packets must never be visible.

Check: mapped round-trip test passes.

### 6.8 Implement buffer ownership states

Enforce `Free → Filling → Ready → InFlight → Free` and reject all other moves.

Check: unit tests cover every transition.

### 6.9 Submit non-blocking transfers

Submit ready batches with OpenCL events; never use queue-wide finish in the
steady-state loop.

Check: second buffer fills while first transfer is in flight.

### 6.10 Poll completions, retain newest result, and profile conditionally

Return a buffer to Free only after its event completes; collect profiling only
in benchmark mode. Poll GPU completion once per replay iteration; publish only a
complete newest `ModelUpdate` to the coordinator.

Check: delayed completion cannot cause buffer overwrite.

### 6.11 Prove the safe GPU-to-RTL hand-off

Use a smoke-kernel-produced test `ModelUpdate`. `ReplayCoordinator` must reject
non-finite/out-of-range/incomplete results, write all nine values through
`VerilatorRunner` into the RTL shadow bank, and commit only at an event boundary.

Check: a complete update changes RTL model version once; an incomplete or invalid
update cannot affect a signal.

**Gate:** portable OpenCL transfers are safe and asynchronous, the eight-feature input
and nine-value update schemas are proven, and a complete test update reaches RTL only
through an atomic commit.

## Phase 7 — Pluggable GPU learning model and parameter updates

### 7.1 Implement the pending-label queue

Maintain a 32-entry valid-feature sequence ring. Store each complete sequence,
its current eight-feature vector, reference midpoint, and target event index.
Reset sequence collection whenever features are invalid.

Check: label cannot be produced before horizon expiry.

### 7.2 Implement label completion

Future midpoint rise is label 1; fall is 0; ties/invalid states are omitted.

Check: rise, fall, tie, empty book, and horizon boundary tests.

### 7.3 Implement a CPU oracle for the selected model

Implement deterministic forward/backward behaviour for the chosen model. The initial
model may be ordinary linear or logistic regression. It consumes the Phase 6 input
layout and produces only a Phase 6 `ModelUpdate`; it does not know about Verilog.

Check: hand-worked prediction, loss, gradient, generated-update, and batch vectors.

### 7.4 Implement GPU prediction/update kernels for the selected model

Keep learner parameters device-resident. Implement forward, backward, update, and
latest-sequence inference kernels; calculate compact loss/accuracy metrics and return
the Phase 6 `ModelUpdate` packet.

Check: CPU/GPU generated values, prediction, gradient, and one-update agreement
within documented tolerance.

### 7.5 Fill and submit labelled batches

Preserve event order; submit full batches only in v1 and report tail samples.

Check: buffer swap cannot reorder samples.

### 7.6 Keep replay running during GPU work

Poll completions once per event loop iteration with no blocking finish/wait.

Check: timing instrumentation contains no steady-state global wait.

### 7.7 Schedule GPU `ModelUpdate` readback

After every completed training batch, run inference on the latest valid sequence
and enqueue one complete `ModelUpdate` readback tied to the batch sequence.

Check: no early or duplicate readbacks.

### 7.8 Validate and convert generated adjustments

Reject NaN/infinity. Clamp eight generated weight adjustments and one bias
adjustment to documented bounds, convert them to saturated Q16.16, and record
clamp/saturation counts. Keep FPGA action thresholds fixed in this version.

Check: injected invalid value never reaches RTL.

### 7.9 Hand the update to the Phase 6 coordinator contract

Pass the complete packet to `ReplayCoordinator`; reuse the Phase 6 validation, shadow
bank write, and atomic-commit path. Do not duplicate RTL hand-off logic inside the
learner.

Check: no signal contains a mixed model.

### 7.10 Add end-to-end adaptive test

Use deterministic data for two batches and one generated-parameter commit; assert
model version, bounded conversion, and a following RTL score change. Compare against
the selected CPU oracle as a reported experiment.

Check: GPU test skips explicitly on hosts without supported GPU.

**Gate:** validated GPU learning changes FPGA-model behavior only atomically,
while the FPGA continues event processing without waiting for GPU work.

## Phase 8 — Dashboard

### 8.1 Define versioned runtime snapshot JSON

Include timestamp, cycle, book arrays, features, signal, model version,
weights, learning metrics, and performance metrics.

Check: serialization test has stable field names/types.

### 8.2 Create immutable snapshots

Copy latest completed runtime state for observers; never expose mutable book or
parameter data to networking code.

Check: snapshot operation cannot change processing checksums.

### 8.3 Add Boost.Beast HTTP/WebSocket server

Serve static files and accept localhost WebSocket clients on configured port.

Check: scripted client connects and receives one payload.

### 8.4 Add bounded output queues

Keep only newest pending snapshot and disconnect persistently slow clients.

Check: slow client cannot produce unbounded memory use.

### 8.5 Add 10 Hz publisher

Publish from a timer, never per event.

Check: message count respects configured rate.

### 8.6 Build dashboard panels

Implement book depth, features, signal, weights, loss/accuracy, and throughput
panels using native HTML/CSS/JavaScript.

Check: fixture payload visibly distinguishes invalid feature from HOLD.

### 8.7 Test disconnect safety

Connect, disconnect, and stall clients during replay.

Check: replay finishes with identical checksum and no dashboard client.

**Gate:** dashboard remains purely observational.

## Phase 9 — Benchmarking and release

### 9.1 Define benchmark metadata

Write commit, configuration, seed, OS/CPU/compiler, OpenCL platform/device,
and trace/dashboard state into every CSV row.

Check: benchmark rejects missing metadata.

### 9.2 Benchmark RTL simulation

Measure cycles/event, events/s, feature/signal latency, and trace overhead.

Check: repeated runs record median/min/max.

### 9.3 Benchmark transfers and learning

Compare pinned/pageable, synchronous/asynchronous, and batch sizes 256, 512,
1,024, 2,048, and 4,096.

Check: raw CSV preserves every run.

### 9.4 Benchmark end-to-end modes

Compare reference-only, RTL-only, RTL+GPU, dashboard off/on, and trace off/on.

Check: unavailable modes are labelled unavailable, never silently substituted.

### 9.5 Add plotting scripts

Generate charts from CSV only.

Check: deleting plots and regenerating reproduces them.

### 9.6 Complete documentation

Write architecture, protocol, order-book, fixed-point, OpenCL buffers,
dashboard, build/troubleshooting, results, and limitations docs.

Check: every formula and external interface has linked test coverage.

### 9.7 Perform clean-environment validation

Build from a clean Ubuntu install following documentation only.

Check: reference demo, RTL tests, and device enumeration work.

### 9.8 Run release gate

Run C++/RTL/Python tests, long differential test, GPU integration if available,
sanitizers, demo, and benchmarks. Archive checksums and raw CSV.

Check:

```bash
./scripts/run_tests.sh
./scripts/run_demo.sh
./scripts/run_benchmark.sh
```

## Stop-and-fix conditions

Do not continue to the next numbered step if C++ and RTL diverge, a fixed-point
golden vector fails, a buffer reaches an illegal state, a non-finite weight is
seen, a partial model can commit, or the dashboard affects replay completion.
Add a regression test before continuing.
