# Directed replay fixtures

These small canonical CSV streams exercise common order-book outcomes and are
intended for fast manual replay or future integration tests.

| Fixture | Expected final checksum | Purpose |
| --- | --- | --- |
| `balanced_book.csv` | `0x1953aafbc1d0614a` | Add both best levels, then partially trade the ask. |
| `cancel_removes_level.csv` | `0xdf30f533ba8376d6` | Cancel an entire bid level and verify that its slot becomes unused. |

The checksum is the deterministic C++ reference checksum with the default
model parameters. Regenerate it with:

```bash
./build/market_engine_demo --reference-only --input tests/fixtures/balanced_book.csv
```
