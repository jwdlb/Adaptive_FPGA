# Order book

The RTL pipeline and the C++ reference model use ten visible aggregated levels
per side.

```text
Bids: highest first             Asks: lowest first
10000 x 50                      10002 x 30
 9999 x 80                      10003 x 60
```

The best bid is the highest available buyer price. The best ask is the lowest
available seller price. They are quotes, not a guaranteed actual trade price.

Supported events are Add, Update, Cancel, and Trade. Unless crossed books are
explicitly allowed, an event that makes best bid greater than or equal to best
ask is rejected without changing the book.

For full semantics and feature definitions, see
[reference_model.md](reference_model.md).

## Why the book matters to training

The order book supplies both kinds of information in this project:

```text
book state now
  -> eight derived features       -> GPU model input
  -> best bid / best ask          -> later executable-outcome label
```

The midpoint is useful inside some feature calculations, but it is only the
average of the best quote prices. The Phase 7 label instead uses the more
practical question of whether a BUY or SELL could have cleared the required
profit using future executable top-of-book prices.
