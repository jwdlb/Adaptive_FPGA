# SystemVerilog RTL

The RTL implements the operational order-book-to-signal pipeline used by the
C++ application through Verilator.

```text
market_stream_adapter
└─ market_pipeline
   ├─ order_book
   ├─ feature_window
   ├─ feature_engine
   └─ strategy_model
```

Shared event, book, feature and model types live in `market_types_pkg.sv`;
fixed-point helpers live in `fixed_point_pkg.sv`.

## Interfaces

- Input uses a `valid/ready` `market_event_t` handshake.
- `market_stream_adapter` holds one complete compact result until
  `result_valid && result_ready`.
- Model parameters arrive as ten indexed Q16.16 writes into a shadow bank plus
  one atomic `param_commit` at an idle boundary.
- Book and feature snapshots are exposed for the Verilator host and tests.

The current depth is ten levels per side, the rolling window is 64 events, and
the feature vector has eight values. These are compiled RTL properties, not
runtime JSON dimensions.

## Build and test

Use the repository CMake targets so source ordering and wrapper modules remain
consistent:

```bash
./scripts/build.sh
cmake --build build --target rtl_tests
cmake --build build --target rtl_stream_adapter_tests
cmake --build build --target rtl_differential_test
```

The differential target feeds a deterministic million-event stream through
both the RTL order book and independent C++ implementation and compares their
state per event.

See [the architecture guide](../docs/architecture.md),
[order-book semantics](../docs/order_book.md), and
[GPU-to-RTL updates](../docs/gpu_to_rtl.md).
