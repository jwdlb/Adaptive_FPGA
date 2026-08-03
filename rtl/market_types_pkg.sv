// Shared protocol types for the Phase 4 RTL order book.
// Keep encodings and widths aligned with include/market/event.hpp.
package market_types_pkg;

  // compile-time configurable constant
  parameter int unsigned BOOK_DEPTH = 10;

  // creates a type 'event_type_t' that can hold one of the four named event values.
  // - typedef creates a resuable type alias (i.e. it gives a name to this type, event_type_t)
  // - enum defines a fixed set of allowed values
  // - logic [1:0] specifies the underlying representation of the enum as a 2-bit logic vector

  typedef enum logic [1:0] {
    EVENT_ADD    = 2'd0,
    EVENT_UPDATE = 2'd1,
    EVENT_CANCEL = 2'd2,
    EVENT_TRADE  = 2'd3
  } event_type_t;

  // creates a type 'side_t' that can hold one of the two named side values.
  // Only one bit so no need for a vector, but we still use logic to avoid synthesis issues with 'bit' type.
  typedef enum logic {
    SIDE_BID = 1'b0,
    SIDE_ASK = 1'b1
  } side_t;

  // creates a type 'book_error_t' that can hold one of the five named error values.
  typedef enum logic [2:0] {
    BOOK_ERROR_NONE                = 3'd0,
    BOOK_ERROR_INVALID_EVENT       = 3'd1,
    BOOK_ERROR_MISSING_PRICE       = 3'd2,
    BOOK_ERROR_CROSSED_BOOK        = 3'd3,
    BOOK_ERROR_INVARIANT_VIOLATION = 3'd4
  } book_error_t;

  // creates a type 'market_event_t' that represents a market event.
  // - typedef creates a resuable type alias (i.e. it gives a name to this type, market_event_t)
  // - struct defines a composite type that groups together multiple fields
  // - packed means that it stores all fields as one contiguous bit vector
  typedef struct packed {
    logic [63:0]       timestamp_ns;
    event_type_t        event_type;  // two bits wide, as defined in event_type_t
    side_t              side;  // one bit wide, as defined in side_t
    logic signed [31:0] price_ticks;
    logic        [31:0] quantity;
  } market_event_t;

  // creates a type 'price_level_t' that represents a price level in the order book.
  typedef struct packed {
    logic signed [31:0] price_ticks;
    logic        [31:0] quantity;
  } price_level_t;

  // Function to check if a market event is formed correctly.
  // - function describes combination computation that can be reused in multiple places
  // - automatic means that the function can be called recursively as each call has it's onw stack frame (local storage)
  // - logic means that the function returns a single bit value (1 valid or 0 invalid)
  // - input market_event_t candidate means that the function takes a single argument of type market_event_t
  function automatic logic event_is_well_formed(input market_event_t candidate);
    // If the price is zero or negative, the event is invalid.
    if (candidate.price_ticks <= 0) begin
      return 1'b0;
    end

    // If the quantity is zero for an add, cancel, or trade event, the event is invalid.
    if ((candidate.event_type == EVENT_ADD) ||
        (candidate.event_type == EVENT_CANCEL) ||
        (candidate.event_type == EVENT_TRADE)) begin
      return candidate.quantity != 0;
    end

    // Update with quantity zero is valid: it removes a visible level.
    return 1'b1;
  endfunction

  // Saturated add function, instead of wrapping around sum after overflow, it “sticks” at the maximum.
  // - return value is 32-bits UNSIGNED logic vector
  function automatic logic [31:0] saturated_add_u32(input logic [31:0] left, input logic [31:0] right);
    logic [32:0] sum;   // Making a 33-bit temproary variable in case addition overflows 32 bits
    sum = {1'b0, left} + {1'b0, right};   // Adds two numbder safely at 33-bit width ({x, y} is concatenation operator)
    return sum[32] ? 32'hffff_ffff : sum[31:0];    // if the 33rd bit is set, it means overflow happened, so return max value, else return the sum
  endfunction

  // Function to decide how much quantity is removed from the book when a cancel or trade event is processed.
  // - visible is the quantity currently visible in the book at that price level
  // - requested is the quantity requested to be removed by the cancel or trade event
  function automatic logic [31:0] removed_quantity(input logic [31:0] visible, input logic [31:0] requested);
    return (requested >= visible) ? visible : requested;   // returns the smaller of the two values, ensuring we don't remove more than is visible
  endfunction

endpackage
