# C++ reference model semantics

The reference order book has ten visible aggregated levels per side. Bids are
strictly descending by price and asks are strictly ascending. An unused level
has both price and quantity set to zero. Unless `allowCrossedBooks` is enabled,
an event that would make best bid greater than or equal to best ask is rejected
without changing the book.

`Add` aggregates a matching price, or inserts a new level and drops the worst
visible level when the book is full. `Update` changes an existing level and an
update to zero removes it. `Cancel` and `Trade` subtract at most the visible
quantity and remove an empty level. Missing-price Update, Cancel, and Trade
events are errors.

The feature vector contains signed Q16.16 values in this order:

1. normalized spread: `(best_ask - best_bid) / midpoint`;
2. L1 imbalance: `(best_bid_quantity - best_ask_quantity) / L1_quantity`;
3. L10 imbalance: `(total_bid_quantity - total_ask_quantity) / total_quantity`;
4. normalized microprice delta: `(microprice - midpoint) / midpoint`;
5. 64-event order-flow sum divided by total visible quantity;
6. 64-event trade-flow sum divided by total visible quantity;
7. mean absolute midpoint movement divided by midpoint; and
8. constant bias `1.0`.

Features are invalid while either best side is absent. The linear strategy
multiplies each feature by its Q16.16 weight, accumulates with 64-bit
intermediates, and saturates to Q16.16. It buys only when the score is strictly
greater than the buy threshold and sells only when strictly less than the sell
threshold; equality produces Hold.
