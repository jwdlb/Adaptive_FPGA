# CSV / binary data to RTL

This is the normal input path. The loaded event vector is already the backlog,
so there is deliberately no extra C++ input queue or RTL input FIFO.

```text
CSV or MKT1 file
       |
       v
io::read_events()
       |
       v
vector<market::MarketEvent>
       |
       v
VerilatorWorker holds the next event until RTL says in_ready
       |
       v
market_stream_adapter valid/ready input
       |
       v
market_pipeline
```

`MarketEvent` contains timestamp, event type, side, integer price ticks, and
quantity. See [protocol.md](protocol.md) for the exact file format.

The RTL worker is the only code allowed to advance the simulated RTL clock. It
does not wait for GPU training before offering another event.
