# Market protocol and numerical rules

Version 1 uses decoded, single-instrument market events. The five fields are
timestamp in nanoseconds, event type, side, integer price ticks, and unsigned
quantity. Event type encodings are Add=0, Update=1, Cancel=2, Trade=3. Side
encodings are Bid=0 and Ask=1.

Prices must be positive. Add, Cancel, and Trade require non-zero quantity.
Update is allowed to have zero quantity and removes the matching level.

CSV requires this exact header:

    timestamp_ns,event_type,side,price_ticks,quantity

Example records:

    1000,Add,Bid,10001,500
    1010,Trade,Ask,10002,100

The binary format is explicit little-endian, never native C++ memory layout.
Its 16-byte header is ASCII MKT1, uint16 version 1, uint16 header size 16,
uint32 record size 20, and uint32 record count. Each 20-byte record is uint64
timestamp, uint8 event type, uint8 side, uint16 zero reserved bytes, int32
price ticks, and uint32 quantity. Truncation, unknown layouts, invalid events,
and trailing bytes are errors.

Normalized features and model values use signed Q16.16 numbers: real value
multiplied by 65,536, so 1.0 is stored as 65,536. Calculations use widened
64-bit intermediates, round nearest with exact halves away from zero, and
saturate to signed 32-bit output. NaN, infinity, and division by zero are
errors.

The compact `RtlStreamResult` is an internal host boundary rather than a stable
wire/file protocol. It contains event index/timestamp, error, feature validity,
eight Q16.16 features and best bid/ask context used only for labelling.
