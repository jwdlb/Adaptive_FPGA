`timescale 1ns/1ps

// Small configuration test: crossed books are accepted when the module
// parameter ALLOW_CROSSED_BOOKS is enabled.
module order_book_allow_crossed_tb;
  import market_types_pkg::*;

  logic clk = 1'b0;
  logic rst_n = 1'b0;
  logic in_valid;
  logic in_ready;
  market_event_t in_event;
  logic event_done;
  book_error_t event_error;
  logic signed [63:0] order_flow_delta;
  logic signed [63:0] trade_flow_delta;
  price_level_t bid_snapshot [0:BOOK_DEPTH-1];
  price_level_t ask_snapshot [0:BOOK_DEPTH-1];

  always #5 clk = ~clk;

  order_book #(.ALLOW_CROSSED_BOOKS(1'b1)) dut (
    .clk, .rst_n, .in_valid, .in_ready, .in_event,
    .event_done, .event_error, .order_flow_delta, .trade_flow_delta,
    .bid_snapshot, .ask_snapshot
  );

  task automatic submit_event(input market_event_t market_event);
    while (!in_ready) @(posedge clk);
    @(negedge clk);
    in_event = market_event;
    in_valid = 1'b1;
    @(posedge clk);
    @(negedge clk);
    in_valid = 1'b0;
    while (!event_done) @(posedge clk);
  endtask

  initial begin
    in_valid = 1'b0;
    in_event = '0;
    repeat (2) @(posedge clk);
    rst_n = 1'b1;
    @(posedge clk);

    submit_event('{64'd1, EVENT_ADD, SIDE_ASK, 32'sd103, 32'd10});
    submit_event('{64'd2, EVENT_ADD, SIDE_BID, 32'sd103, 32'd10});

    assert (event_error == BOOK_ERROR_NONE)
      else $fatal(1, "crossed book was rejected despite ALLOW_CROSSED_BOOKS");
    assert (bid_snapshot[0].price_ticks == 32'sd103)
      else $fatal(1, "crossed bid was not committed");

    $display("ALLOW_CROSSED_BOOKS test passed.");
    $finish;
  end
endmodule
