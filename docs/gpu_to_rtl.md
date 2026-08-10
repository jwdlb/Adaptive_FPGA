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

The RTL worker rejects a non-newer version or invalid BUY/SELL threshold
ordering before any RTL write. It then writes all ten values to its shadow bank,
and commits them together. A signal therefore uses the old model or the full
new model, never a mixture.

## Why a mailbox, not a normal queue?

Only the newest complete model matters. If the GPU creates model version 12
and then version 13 before RTL has applied version 12, applying 12 first adds
delay without improving the system. The mailbox can safely retain 13 instead.

```text
empty -> GPU publishes complete model -> ready -> RTL takes it -> empty
```

The mutex is only around this tiny model packet. It is not on the high-rate
RTL-to-GPU result path, which uses the lock-free SPSC ring instead.

## Atomic RTL application

```text
write weight 0 ┐
write weight 1 │
...            │ shadow parameter bank only
write SELL     ┘
       |
       v
commit once -> all ten values become active together
```

The commit is attempted only when no held result is waiting and the core is
ready for another event. After the input stream drains, the RTL worker remains
alive until the GPU finishes its last full batch and the coordinator closes
the mailbox, so a final update is not lost during shutdown.
