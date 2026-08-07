`timescale 1ns/1ps

// Flat scalar-port wrapper for the C++/Verilator differential test. The order-book
// core remains unchanged; this wrapper only makes its packed input and snapshots
// straightforward to access from generated C++ simulation code.
//
// C++ drives simple values such as `price_ticks` and `quantity`. This wrapper packs
// them into the `market_event_t` record expected by order_book.sv. In the other
// direction, it exposes each bid/ask snapshot level as one 64-bit value so the C++
// test can compare it with the C++ reference book.
module order_book_cpp_wrapper (
  // Clock, reset, and valid/ready handshake signals used to drive the real RTL.
  input logic clk,
  input logic rst_n,
  input logic in_valid,
  output logic in_ready,
  // Flat event fields supplied directly by the C++ differential test.
  input logic [63:0] timestamp_ns,
  input logic [1:0] event_type,
  input logic side,
  input logic signed [31:0] price_ticks,
  input logic [31:0] quantity,
  // Completion/error/flow outputs forwarded unchanged from the order-book core.
  output logic event_done,
  output market_types_pkg::book_error_t event_error,
  output logic signed [63:0] order_flow_delta,
  output logic signed [63:0] trade_flow_delta,
  // The twenty snapshot outputs. Each 64-bit value is the packed price_level_t
  // record: price_ticks occupies the upper 32 bits and quantity the lower 32 bits.
  output logic [63:0] bid_level0,
  output logic [63:0] bid_level1,
  output logic [63:0] bid_level2,
  output logic [63:0] bid_level3,
  output logic [63:0] bid_level4,
  output logic [63:0] bid_level5,
  output logic [63:0] bid_level6,
  output logic [63:0] bid_level7,
  output logic [63:0] bid_level8,
  output logic [63:0] bid_level9,
  output logic [63:0] ask_level0,
  output logic [63:0] ask_level1,
  output logic [63:0] ask_level2,
  output logic [63:0] ask_level3,
  output logic [63:0] ask_level4,
  output logic [63:0] ask_level5,
  output logic [63:0] ask_level6,
  output logic [63:0] ask_level7,
  output logic [63:0] ask_level8,
  output logic [63:0] ask_level9
);
  import market_types_pkg::*;

  // The structured event and snapshot arrays used by the actual RTL order book.
  market_event_t in_event;
  price_level_t bid_snapshot [0:BOOK_DEPTH-1];
  price_level_t ask_snapshot [0:BOOK_DEPTH-1];

  // Translate the flat C++ values into the packed RTL event and flatten the RTL
  // snapshot arrays back into individually named C++-friendly outputs.
  always_comb begin
    in_event = '{timestamp_ns: timestamp_ns, event_type: event_type_t'(event_type),
                 side: side_t'(side), price_ticks: price_ticks, quantity: quantity};
    bid_level0 = bid_snapshot[0]; bid_level1 = bid_snapshot[1];
    bid_level2 = bid_snapshot[2]; bid_level3 = bid_snapshot[3];
    bid_level4 = bid_snapshot[4]; bid_level5 = bid_snapshot[5];
    bid_level6 = bid_snapshot[6]; bid_level7 = bid_snapshot[7];
    bid_level8 = bid_snapshot[8]; bid_level9 = bid_snapshot[9];
    ask_level0 = ask_snapshot[0]; ask_level1 = ask_snapshot[1];
    ask_level2 = ask_snapshot[2]; ask_level3 = ask_snapshot[3];
    ask_level4 = ask_snapshot[4]; ask_level5 = ask_snapshot[5];
    ask_level6 = ask_snapshot[6]; ask_level7 = ask_snapshot[7];
    ask_level8 = ask_snapshot[8]; ask_level9 = ask_snapshot[9];
  end

  // Instantiate the real order-book implementation. This wrapper adds no market
  // logic; it only adapts the test interface.
  order_book dut (
    .clk, .rst_n, .in_valid, .in_ready, .in_event, .event_done, .event_error,
    .order_flow_delta, .trade_flow_delta, .bid_snapshot, .ask_snapshot
  );
endmodule
