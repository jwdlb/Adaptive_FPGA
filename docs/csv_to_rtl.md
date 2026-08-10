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

In dashboard control mode, generation and file loading happen on a separate
command worker. A `run` command still loads and validates the complete file
before constructing `LiveCoordinator`; browser data is never streamed directly
into RTL.

## Exact types on the path

```text
file bytes
  -> io::read_events(...)
  -> std::vector<market::MarketEvent>
  -> std::span<const market::MarketEvent>
  -> verilator::VerilatorWorker
  -> RTL market_types_pkg::market_event_t
```

The C++ vector is intentionally immutable while the worker runs. The worker
only remembers an index pointing at the next event. This avoids copying the
entire input stream into another queue.

## One event, simply

```text
1. Worker points at next MarketEvent in the vector.
2. RTL says in_ready.
3. Worker drives that event and raises in_valid for one clock.
4. RTL accepts it; worker advances its vector index.
5. RTL updates the book, rolling window, features, and strategy over later clocks.
6. RTL holds one completed result until C++ accepts it.
```

If the output route is full, the worker stops accepting new outputs safely.
That backpressure eventually stops new RTL input too; it never drops or
overwrites a completed market result. The one-entry RTL register is followed by
a 1,024-entry host SPSC ring; both boundaries participate in backpressure.
