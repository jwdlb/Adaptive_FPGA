# GPU learner

The current learner is a Q16.16 linear-regression baseline. OpenCL evaluates
one training row per work item, then applies a deterministic regularised batch
gradient to device-resident model state.

## Model

```text
score = weight0*feature0 + ... + weight7*feature7
```

Feature 7 is always `1.0`, so weight 7 is the bias/intercept.

## Inputs and labels

The model feature input remains eight values per row:

```text
spread, L1 imbalance, L10 imbalance, microprice delta,
order flow, trade flow, movement signal, constant 1.0
```

The separate label is created from future executable bid/ask prices:

```text
+1 = BUY would clear the required profit
 0 = HOLD / no clear executable opportunity
-1 = SELL would clear the required profit
```

## Training and output

For every row, the GPU calculates a score, compares it with `-1`, `0`, or
`+1`, and produces a gradient contribution. The batch mean gradient and the
configured Q16.16 L2 penalty are then applied to all eight weights.

After a full batch, the GPU retains its updated eight weights and returns a
complete `ModelUpdate` through the mailbox. The current kernels preserve the
active BUY/SELL thresholds; if an invalid threshold ordering reaches the
kernel, it repairs the pair to +0.25/-0.25. Threshold optimisation is not yet
part of training.

## Batch diagram

```text
32, 256, 1024, or another configured N rows

row 0: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label: +1
row 1: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label:  0
row 2: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label: -1
 ...
row N-1: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label:  0
```

All values are signed Q16.16 integers: `65536` means `1.0`. The GPU has two
separate buffers because labels are answers for training, not a ninth feature.

## What happens in one training batch

```text
current weights
      |
      v
calculate score for each row
      |
      v
error = label - score
      |
      v
small adjustment to each weight
      |
      v
updated weights + thresholds + version
```

The first kernel, `regression_row_gradients`, runs one work item per row. It
calculates the score, error, eight row gradients, squared error and a simple
three-way correctness value. The second kernel, `regression_apply_batch`, runs
one work item per coefficient, reduces row gradients in a deterministic order,
applies the mean gradient and L2 penalty, and clamps each weight to ±32.0.

OpenCL profiling events provide upload, kernel, readback and end-to-end update
timings. The reported `kernelMs` spans both kernel stages.

## Batch lifecycle and backpressure

Only one batch is mapped or in flight. While OpenCL is running, `GpuWorker`
polls its completion and does not consume additional SPSC results. If the GPU
is slower than the RTL simulation, ring occupancy rises and eventually
propagates valid/ready backpressure to RTL.

A partial final batch is discarded. The final `labelHorizonEvents` results are
also discarded because a future quote is unavailable. This is why an input
must contain more than the configured horizon plus a full batch to guarantee
an update.

## What it is and is not

It is a real data-parallel GPU learner: OpenCL work items calculate row
gradients, another stage reduces and updates weights, and the GPU retains model
state between batches. It is not a neural network or a guarantee of profitable
trading. Labels omit fees, slippage, fill probability, queue position and
market impact.
