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
