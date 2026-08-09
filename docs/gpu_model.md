# GPU learner

The current learner is a Q16.16 linear regression-style model. It is real
OpenCL GPU code, although its first version uses one work item for predictable
sequential SGD before a later parallel GPU optimisation.

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
`+1`, and applies a small Q16.16 SGD adjustment to all eight weights. The
configured `learningRate` controls that adjustment.

After a full batch, the GPU retains its updated model state, chooses basic
BUY/SELL thresholds from profitable batch scores, and returns a complete
`ModelUpdate` through the mailbox.

## Batch diagram

```text
32, 256, 1024, or another configured N rows

row 0: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label: +1
row 1: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label:  0
row 2: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label: -1
 ...
row N: [ f0 f1 f2 f3 f4 f5 f6 1.0 ]  label:  0
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

The current threshold method is deliberately basic: it examines scores for
profitable BUY and SELL examples and chooses values between HOLD and those
scores. It is a starting point to verify the full architecture; later work can
use a better profit-based threshold search.

## What it is and is not

It is a real GPU learner: an OpenCL kernel performs the score and weight
updates, and the GPU retains its model state between batches. It is not yet a
large neural network, a parallel gradient implementation, or a guarantee of
profitable trading.
