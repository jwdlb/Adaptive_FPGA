# Test suite

Run the normal suite:

```bash
./scripts/build.sh
ctest --test-dir build --output-on-failure
```

It covers C++ order-book rules, RTL/C++ differential replay, a one-million
event replay, the SPSC ring, mailbox ownership, RTL worker behaviour, and the
held RTL result register.

Run the standalone RTL streaming adapter test:

```bash
cmake --build build --target rtl_stream_adapter_tests
```

Run the full RTL order-book differential replay:

```bash
cmake --build build --target rtl_differential_test
```

The GPU smoke and GPU learner tests skip on this WSL machine because it has no
selectable OpenCL GPU. On the RTX 4060 machine they should run and verify the
OpenCL learner path.

## What a successful RTX check should show

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
