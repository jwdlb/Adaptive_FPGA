# Adaptive FPGA–GPU Market Signal Engine

This project is a market-signal research prototype. C++ reads market events,
SystemVerilog RTL maintains an order book and calculates features, and an
OpenCL GPU learner fine-tunes a model which RTL can atomically adopt.

The RTL currently runs through **Verilator simulation**, not a physical FPGA.
The C++ reference/replay model is test support, not the normal live path.

## System overview

```text
 CSV / MKT1 market data
          |
          v
 Event reader -> vector<MarketEvent> -> dedicated VerilatorWorker
                                             |
                                             | direct valid/ready input
                                             v
 +-------------------------- RTL -----------------------------------+
 | order book -> feature engine -> strategy -> held output register |
 +-------------------------------------------------------------------+
                                             |
                                             | RtlStreamResult
                                             v
              SPSC ring: one RTL producer, one GPU consumer
                                             |
                                             v
 +----------------------- GPU worker -------------------------------+
 | delayed labels -> mapped [N][8] features + [N] labels            |
 +-------------------------------------------------------------------+
                                             |
                                             v
          OpenCL GPU learner -> complete ModelUpdate
                                             |
                                             v
        ModelUpdateMailbox -> RTL shadow bank -> atomic commit
```

RTL and GPU code never call each other directly. C++ owns the ring, mailbox,
validation, and shutdown between them.

## Read the system by data path

- [CSV / binary data into RTL](docs/csv_to_rtl.md)
- [RTL results into the GPU](docs/rtl_to_gpu.md)
- [GPU model updates back into RTL](docs/gpu_to_rtl.md)
- [GPU learner: model, inputs, labels, and outputs](docs/gpu_model.md)
- [Order book behaviour](docs/order_book.md)
- [Test suite and commands](docs/test_suite.md)
- [Event format and fixed-point rules](docs/protocol.md)
- [C++ reference-model semantics](docs/reference_model.md)

## Current status

- The normal path is CSV → RTL → SPSC → GPU → mailbox → RTL.
- The first GPU learner is implemented as Q16.16 linear SGD.
- GPU execution is unverified on this WSL machine because it has no selectable
  OpenCL GPU; verify it on the RTX 4060 machine.
- `featureBatchSize` is the GPU batch size `N`; `labelHorizonEvents` is the
  future look-ahead distance.

## Build and run

```bash
./scripts/build.sh
./scripts/run_tests.sh
./build/market_engine_demo --input data/events.csv
```

On the RTX/OpenCL machine, use enough valid events for one labelled batch:

```bash
./build/market_engine_demo \
  --input data/events.csv \
  --gpu-feature-upload \
  --batch-size 1024
```

The existing `--gpu-feature-upload` command name now starts the full streaming
GPU learner, not merely a copy test.
