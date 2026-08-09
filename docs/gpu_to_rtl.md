# GPU to RTL model-update path

```text
OpenCL learner finishes training
       |
       v
complete gpu::ModelUpdate
       |
       v
ModelUpdateMailbox
       |
       v
VerilatorWorker at a safe boundary
       |
       v
RTL shadow parameter bank -> atomic commit
```

The GPU never writes RTL registers directly. The mailbox holds only the newest
complete replacement model:

```text
weights[0..6]   market-feature weights
weights[7]      bias/intercept
buy_threshold
sell_threshold
update_version
```

The RTL worker validates the update, writes all ten values to its shadow bank,
and commits them together. A signal therefore uses the old model or the full
new model, never a mixture.
