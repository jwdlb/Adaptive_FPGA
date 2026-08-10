# Dashboard and telemetry

The application embeds a small dependency-free browser dashboard in
`src/app/dashboard_server.cpp`. It presents current replay state and can queue
data-generation/replay jobs when the binary is running without an input file.

![Dashboard preview](dashboard_preview.svg)

The preview uses illustrative values to show the layout; live values are
delivered by the running application.

## Start the UI

Persistent control mode keeps the server open until `SIGINT` or `SIGTERM`:

```bash
./build/market_engine_demo
# http://127.0.0.1:8080/
```

A finite input replay also exposes the dashboard, but only for the lifetime of
that run:

```bash
./build/market_engine_demo --input data/events.csv
```

Disable the web server and snapshot publisher entirely with `--no-dashboard`.

## UI panels

| Panel | Content |
| --- | --- |
| Controls | Output/input paths, event count, random seed, optional GPU training and job progress |
| Overview | Replay throughput, processed/error counts, SPSC occupancy and GPU batch/update counts |
| Signal | Current BUY/HOLD/SELL action and raw/decoded Q16.16 score |
| Top of book | Current best non-empty bid and ask |
| RTL features | Relative magnitudes of the eight fixed-point features |
| Model | Active version, thresholds, latest batch loss/accuracy, update time and all eight weights |
| Performance | RTL cycles/event, GPU training time, update latency and connected clients |
| Event tape | The most recent sampled events in reverse chronological order |

Dashboard book and event-tape data is sampled, not a lossless audit log.

## Network API

### `GET /`

Returns the embedded HTML, CSS and JavaScript application. `GET /index.html`
is equivalent. Responses carry `Cache-Control: no-store`.

### `GET /health`

Returns:

```json
{"ok":true}
```

This proves that the HTTP listener is responsive; it does not assert that a
replay or GPU is healthy.

### `GET /ws`

Upgrades to a WebSocket. The server sends a complete JSON snapshot at the
configured dashboard frequency. Client messages are accepted and discarded;
the WebSocket is read-only from an application-control perspective.

The browser reconnects one second after a disconnected session. Each server
session retains one outbound JSON message and fetches the newest snapshot only
after the preceding write completes. A slow client therefore observes fewer
states instead of creating an unbounded server queue.

### `POST /api/control`

Available only in persistent control mode. The request body must be a JSON
object. Accepted commands enter an eight-entry FIFO and run serially.

Generate deterministic CSV:

```json
{
  "action": "generate",
  "output": "data/dashboard-events.csv",
  "events": 100000,
  "seed": 42
}
```

Replay an input:

```json
{
  "action": "run",
  "input": "data/dashboard-events.csv",
  "events": 50000,
  "gpu": false
}
```

The endpoint returns HTTP `202 Accepted` when queued. Invalid commands return
`400`; finite-run dashboard servers, which have no control handler, return
`503`. The request-body limit is 64 KiB.

## Snapshot schema

Every WebSocket message is a complete schema-versioned object. Major groups
are shown below; fields are additive within schema version 1.

```json
{
  "schemaVersion": 1,
  "sequence": 123,
  "publishedAtUnixMs": 1786320000000,
  "connectionState": "connected",
  "state": "running",
  "activity": {
    "state": "running",
    "message": "Replaying market events through RTL",
    "completed": 4096,
    "total": 100000,
    "queued": 0
  },
  "processedEvents": 4096,
  "errorEvents": 0,
  "eventsPerSecond": 0.0,
  "queue": {"occupancy": 0, "capacity": 1024},
  "book": {"bids": [], "asks": []},
  "bookDepth": {"capacity": 10, "bidLevels": 0, "askLevels": 0},
  "featuresQ16": [0, 0, 0, 0, 0, 0, 0, 65536],
  "featuresValid": true,
  "signal": {"scoreQ16": 0, "action": "Hold", "version": 1},
  "model": {
    "weightsQ16": [0, 0, 0, 0, 0, 0, 0, 0],
    "buyThresholdQ16": 13107,
    "sellThresholdQ16": -13107,
    "version": 1
  },
  "gpu": {},
  "performance": {},
  "dashboardClients": 1,
  "recentEvents": []
}
```

Q16.16 fields are transported as integers to preserve their exact RTL values.
The UI divides them by 65,536 only for human-readable display.

## Telemetry production

The hot path does not serialize JSON or perform socket work. Instead:

1. `VerilatorWorker` samples its state every 256 published results.
2. The coordinator copies it into a `DashboardSnapshot` under a short mutex.
3. A publisher thread publishes the newest copy at `dashboardUpdateHz`.
4. WebSocket sessions copy the current immutable snapshot and serialize it.

GPU metric callbacks update the observed snapshot after batch submission or
completion. UI values from different workers are therefore operational
telemetry, not a transactionally consistent hardware trace.

## Security and deployment

The server has no authentication, authorization, TLS, origin validation or
CSRF protection. Paths supplied through the control endpoint are interpreted
by the local process and can create/overwrite generated CSV output where that
process has permission.

Keep `dashboardBindAddress` at `127.0.0.1` for local development. If remote
access is required, place the service behind a trusted authenticated TLS
reverse proxy and restrict filesystem/process permissions. Do not expose the
control endpoint directly to an untrusted network.

