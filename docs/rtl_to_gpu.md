# RTL to GPU data path

```text
RTL held result register
       |
       v
VerilatorRunner::read_stream_result_into()
       |
       v
RtlStreamResult written directly into a reserved SPSC slot
       |
       v
SpscRingBuffer<RtlStreamResult>
       |
       v
GpuWorker copies one result, then immediately releases the slot
```

The SPSC ring has one producer, `VerilatorWorker`, and one consumer,
`GpuWorker`. It uses atomic positions rather than a whole-queue mutex.

Each result contains event metadata, an error, eight Q16.16 features, and the
best bid/ask price and quantity. Bid/ask values are label context, not extra
RTL strategy features.

When a row becomes `labelHorizonEvents` old, the GPU worker makes its label:

```text
future best bid - old best ask >= 1 tick  -> BUY  (+1)
old best bid - future best ask >= 1 tick  -> SELL (-1)
otherwise                                 -> HOLD (0)
```

The GPU receives mapped OpenCL buffers:

```text
features: [N][8] Q16.16
labels:   [N]    Q16.16
```

`N` is `featureBatchSize`, normally 1024. The GPU never reads directly from a
ring slot.

Only one OpenCL batch is in flight. While that batch runs, `GpuWorker` polls
for completion and temporarily stops consuming the ring. This is deliberate:
the bounded ring exposes GPU pressure instead of hiding it in an unbounded
host queue.

## Ownership picture

```text
RTL worker owns a result
       |
       | reserve SPSC slot, write it, publish it
       v
GPU worker temporarily owns a readable SPSC slot
       |
       | copy result into pending-label storage or mapped OpenCL memory
       | finish_pop: slot is free immediately
       v
GPU worker owns mapped OpenCL memory
       |
       | fill one labelled row in [N][8] and [N]
       v
GPU owns unmapped OpenCL memory while kernel runs
```

This distinction matters: the GPU can train for much longer than it takes to
copy one tiny `RtlStreamResult`. It must never hold the SPSC slot while training
or it would block RTL before the ring's real capacity is used.

The last horizon-sized tail cannot receive labels, and a final partial batch is
discarded during orderly shutdown.

## What is inside one result

```text
verilator::RtlStreamResult
├─ uint64 event_index
├─ uint64 timestamp_ns
├─ market::BookError error
├─ market::FeatureVector
│  ├─ bool valid
│  └─ array<int32, 8> values       (Q16.16)
└─ top-of-book label context
   ├─ bid price / quantity
   └─ ask price / quantity
```
