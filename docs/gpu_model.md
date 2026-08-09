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
