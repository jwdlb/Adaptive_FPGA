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
