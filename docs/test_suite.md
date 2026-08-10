# Test suite

Run the normal suite:

```bash
./scripts/build.sh
ctest --test-dir build --output-on-failure
```

It covers configuration and protocols, C++ order-book/reference semantics,
dashboard snapshots and HTTP handling, model persistence, RTL/C++ replay,
SPSC and mailbox ownership, RTL worker behavior, GPU protocol/model behavior,
and the held RTL result register.

Run the standalone RTL streaming adapter test:

```bash
cmake --build build --target rtl_stream_adapter_tests
```

Run the full RTL order-book differential replay:

```bash
cmake --build build --target rtl_differential_test
```

GPU-dependent tests clearly skip when the current host has no selectable
OpenCL GPU. A skip proves neither success nor failure of GPU execution; run the
same suite on every target driver/device combination.

## What a successful GPU-host check should show

```text
1. GPU smoke test passes: [1,2,3] becomes [2,4,6].
2. GPU learner test no longer says Skipped.
3. A long live input produces one or more GPU batches.
4. GPU model updates are published.
5. RTL model updates are applied.
6. Later RTL signals report a newer model version.
```

Use a file with more valid events than `labelHorizonEvents + featureBatchSize`;
the default values mean more than about 1,124 usable rows.

## Test layers

| Layer | Purpose |
| --- | --- |
| Catch2 unit/integration tests | C++ types, config, IO, runtime ownership, API and model behavior |
| Standalone RTL tests | Module-level clocked assertions and expected outputs |
| RTL/C++ differential replay | Feeds the same deterministic stream to independent implementations and compares every event |
| OpenCL tests | Device selection, upload/execute/readback and training update semantics |

The long differential target is separate from normal CTest so developers can
choose the appropriate runtime cost during iteration.
