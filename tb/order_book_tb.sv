`timescale 1ns/1ps

// Minimal Phase 4 testbench. Add directed and randomized tests here before
// connecting this design to the C++/Verilator differential runner.
//
// This simulation-only file is the driver and checker for rtl/order_book.sv;
// it is not FPGA hardware. It creates a clock, resets the device under test
// (DUT), sends one market event using the valid/ready handshake, and waits for
// the DUT to raise event_done. Add tests here for each order-book operation,
// then check the snapshots, errors, and flow deltas against the C++ OrderBook
// reference so that the RTL and software implementations stay equivalent.
module order_book_tb;
  import market_types_pkg::*;

  logic clk;
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

  initial begin
    clk = 1'b0;
    forever #5 clk = ~clk;
  end

  order_book dut (
    .clk,
    .rst_n,
    .in_valid,
    .in_ready,
    .in_event,
    .event_done,
    .event_error,
    .order_flow_delta,
    .trade_flow_delta,
    .bid_snapshot,
    .ask_snapshot
  );

  task automatic reset_dut;
    rst_n = 1'b0;
    in_valid = 1'b0;
    in_event = '0;
    repeat (2) @(posedge clk);
    rst_n = 1'b1;
    @(posedge clk);
    assert (in_ready) else $fatal(1, "DUT did not become ready after reset");
  endtask

  task automatic submit_event(input market_event_t market_event);
    int unsigned cycles;

    // Wait before asserting valid. Once valid is high, the DUT must see it
    // together with ready at the following rising edge.
    while (!in_ready) @(posedge clk);
    @(negedge clk);
    in_event = market_event;
    in_valid = 1'b1;
    @(posedge clk);
    @(negedge clk);
    in_valid = 1'b0;

    cycles = 0;
    while (!event_done && cycles < 32) begin
      @(posedge clk);
      cycles++;
    end
    assert (event_done) else $fatal(1, "event timed out after %0d cycles", cycles);

    // event_done must be a one-clock pulse, not a level that stays high.
    @(posedge clk);
    @(negedge clk);
    assert (!event_done) else $fatal(1, "event_done was high for more than one cycle");
  endtask

  task automatic submit_held_valid(input market_event_t market_event);
    int unsigned cycles;

    // Hold valid while the DUT is busy. It must not accept the event twice.
    while (!in_ready) @(posedge clk);
    @(negedge clk);
    in_event = market_event;
    in_valid = 1'b1;
    @(posedge clk);
    @(negedge clk);
    assert (!in_ready) else $fatal(1, "DUT stayed ready while processing an event");

    cycles = 0;
    while (!event_done && cycles < 32) begin
      @(negedge clk);
      cycles++;
    end
    assert (event_done) else $fatal(1, "held-valid event timed out after %0d cycles", cycles);

    // Drop valid before the next rising edge, otherwise it would correctly
    // be treated as a second event once the DUT returns to ST_IDLE.
    in_valid = 1'b0;
    assert (in_ready) else $fatal(1, "DUT did not return to ready after completion");

    @(posedge clk);
    @(negedge clk);
    assert (!event_done) else $fatal(1, "event_done was high for more than one cycle");
  endtask

  task automatic expect_bid(
      input int unsigned index,
      input logic signed [31:0] expected_price,
      input logic [31:0] expected_quantity
  );
    assert (bid_snapshot[index].price_ticks == expected_price)
      else $fatal(1, "unexpected bid price at level %0d", index);
    assert (bid_snapshot[index].quantity == expected_quantity)
      else $fatal(1, "unexpected bid quantity at level %0d", index);
  endtask

  task automatic expect_ask(
      input int unsigned index,
      input logic signed [31:0] expected_price,
      input logic [31:0] expected_quantity
  );
    assert (ask_snapshot[index].price_ticks == expected_price)
      else $fatal(1, "unexpected ask price at level %0d", index);
    assert (ask_snapshot[index].quantity == expected_quantity)
      else $fatal(1, "unexpected ask quantity at level %0d", index);
  endtask

  initial begin
    reset_dut();

    // Handshake/backpressure: this Add must be accepted exactly once.
    submit_held_valid('{64'd1, EVENT_ADD, SIDE_BID, 32'sd100, 32'd10});
    expect_bid(0, 32'sd100, 32'd10);

    reset_dut();

    // 1. Add one bid.
    submit_event('{64'd1, EVENT_ADD, SIDE_BID, 32'sd100, 32'd10});
    expect_bid(0, 32'sd100, 32'd10);

    // 2. Add better and worse bids; confirm descending ordering.
    submit_event('{64'd2, EVENT_ADD, SIDE_BID, 32'sd102, 32'd20});
    submit_event('{64'd3, EVENT_ADD, SIDE_BID, 32'sd101, 32'd30});
    expect_bid(0, 32'sd102, 32'd20);
    expect_bid(1, 32'sd101, 32'd30);
    expect_bid(2, 32'sd100, 32'd10);

    // 3. Add an ask, then verify ask ordering separately.
    submit_event('{64'd4, EVENT_ADD, SIDE_ASK, 32'sd103, 32'd15});
    expect_ask(0, 32'sd103, 32'd15);

    // 4. Existing-price Add / aggregation.
    submit_event('{64'd5, EVENT_ADD, SIDE_BID, 32'sd101, 32'd5});
    expect_bid(1, 32'sd101, 32'd35);
    assert (order_flow_delta == 64'sd5)
      else $fatal(1, "incorrect Add flow delta");

    // Update downwards: 35 -> 7 gives -28 bid flow.
    submit_event('{64'd6, EVENT_UPDATE, SIDE_BID, 32'sd101, 32'd7});
    expect_bid(1, 32'sd101, 32'd7);
    assert (order_flow_delta == -64'sd28)
      else $fatal(1, "incorrect Update flow delta");

    // Partial cancel: 7 -> 4 gives -3 bid flow.
    submit_event('{64'd7, EVENT_CANCEL, SIDE_BID, 32'sd101, 32'd3});
    expect_bid(1, 32'sd101, 32'd4);
    assert (order_flow_delta == -64'sd3)
      else $fatal(1, "incorrect Cancel flow delta");

    // Trade the remaining four; the level must be compacted away.
    submit_event('{64'd8, EVENT_TRADE, SIDE_BID, 32'sd101, 32'd10});
    expect_bid(0, 32'sd102, 32'd20);
    expect_bid(1, 32'sd100, 32'd10);
    expect_bid(2, 32'sd0, 32'd0);
    assert (order_flow_delta == -64'sd4)
      else $fatal(1, "incorrect Trade order-flow delta");
    assert (trade_flow_delta == -64'sd4)
      else $fatal(1, "incorrect bid Trade flow delta");

    // A missing update must report an error and leave the book unchanged.
    submit_event('{64'd9, EVENT_UPDATE, SIDE_BID, 32'sd999, 32'd1});
    assert (event_error == BOOK_ERROR_MISSING_PRICE)
      else $fatal(1, "missing Update did not report an error");
    expect_bid(0, 32'sd102, 32'd20);

    // Bid 103 would equal the best ask 103, so it must be rejected atomically.
    submit_event('{64'd10, EVENT_ADD, SIDE_BID, 32'sd103, 32'd1});
    assert (event_error == BOOK_ERROR_CROSSED_BOOK)
      else $fatal(1, "crossed book was not rejected");
    expect_bid(0, 32'sd102, 32'd20);

    // Invalid feed data must be rejected without changing the book.
    reset_dut();
    submit_event('{64'd11, EVENT_ADD, SIDE_BID, 32'sd0, 32'd1});
    assert (event_error == BOOK_ERROR_INVALID_EVENT)
      else $fatal(1, "zero price was not rejected");
    expect_bid(0, 32'sd0, 32'd0);
    submit_event('{64'd12, EVENT_ADD, SIDE_BID, 32'sd100, 32'd0});
    assert (event_error == BOOK_ERROR_INVALID_EVENT)
      else $fatal(1, "zero Add quantity was not rejected");
    submit_event('{64'd13, EVENT_CANCEL, SIDE_BID, 32'sd100, 32'd0});
    assert (event_error == BOOK_ERROR_INVALID_EVENT)
      else $fatal(1, "zero Cancel quantity was not rejected");
    submit_event('{64'd14, EVENT_TRADE, SIDE_BID, 32'sd100, 32'd0});
    assert (event_error == BOOK_ERROR_INVALID_EVENT)
      else $fatal(1, "zero Trade quantity was not rejected");

    // Update-to-zero removes a level and compacts the remaining levels.
    reset_dut();
    submit_event('{64'd15, EVENT_ADD, SIDE_BID, 32'sd102, 32'd20});
    submit_event('{64'd16, EVENT_ADD, SIDE_BID, 32'sd100, 32'd10});
    submit_event('{64'd17, EVENT_UPDATE, SIDE_BID, 32'sd102, 32'd0});
    expect_bid(0, 32'sd100, 32'd10);
    expect_bid(1, 32'sd0, 32'd0);
    assert (order_flow_delta == -64'sd20)
      else $fatal(1, "incorrect Update-to-zero flow delta");

    // Ask operations use the opposite order-flow sign and positive trade flow.
    reset_dut();
    submit_event('{64'd18, EVENT_ADD, SIDE_ASK, 32'sd103, 32'd10});
    submit_event('{64'd19, EVENT_UPDATE, SIDE_ASK, 32'sd103, 32'd12});
    assert (order_flow_delta == -64'sd2)
      else $fatal(1, "incorrect ask Update flow delta");
    submit_event('{64'd20, EVENT_CANCEL, SIDE_ASK, 32'sd103, 32'd3});
    expect_ask(0, 32'sd103, 32'd9);
    assert (order_flow_delta == 64'sd3)
      else $fatal(1, "incorrect ask Cancel flow delta");
    submit_event('{64'd21, EVENT_TRADE, SIDE_ASK, 32'sd103, 32'd20});
    expect_ask(0, 32'sd0, 32'd0);
    assert (order_flow_delta == 64'sd9 && trade_flow_delta == 64'sd9)
      else $fatal(1, "incorrect ask Trade flow deltas");

    // Partial Trade reduces a level without removing it.
    reset_dut();
    submit_event('{64'd22, EVENT_ADD, SIDE_BID, 32'sd100, 32'd10});
    submit_event('{64'd23, EVENT_TRADE, SIDE_BID, 32'sd100, 32'd3});
    expect_bid(0, 32'sd100, 32'd7);
    assert (order_flow_delta == -64'sd3 && trade_flow_delta == -64'sd3)
      else $fatal(1, "incorrect partial Trade flow deltas");

    // An oversized Cancel removes only the quantity that is visible.
    reset_dut();
    submit_event('{64'd24, EVENT_ADD, SIDE_BID, 32'sd100, 32'd5});
    submit_event('{64'd25, EVENT_CANCEL, SIDE_BID, 32'sd100, 32'd20});
    expect_bid(0, 32'sd0, 32'd0);
    assert (order_flow_delta == -64'sd5)
      else $fatal(1, "oversized Cancel removed the wrong quantity");

    // Quantity additions saturate instead of wrapping around to zero.
    reset_dut();
    submit_event('{64'd26, EVENT_ADD, SIDE_BID, 32'sd100, 32'hffff_fffe});
    submit_event('{64'd27, EVENT_ADD, SIDE_BID, 32'sd100, 32'd5});
    expect_bid(0, 32'sd100, 32'hffff_ffff);
    assert (order_flow_delta == 64'sd1)
      else $fatal(1, "saturated Add reported the wrong flow delta");

    // A better price enters a full book; a worse price is ignored.
    reset_dut();
    for (int signed price = 110; price >= 101; price--) begin
      submit_event('{64'd28, EVENT_ADD, SIDE_BID, price, 32'd1});
    end
    expect_bid(0, 32'sd110, 32'd1);
    expect_bid(9, 32'sd101, 32'd1);
    submit_event('{64'd29, EVENT_ADD, SIDE_BID, 32'sd111, 32'd1});
    expect_bid(0, 32'sd111, 32'd1);
    expect_bid(9, 32'sd102, 32'd1);
    submit_event('{64'd30, EVENT_ADD, SIDE_BID, 32'sd100, 32'd1});
    expect_bid(9, 32'sd102, 32'd1);
    assert (order_flow_delta == 64'sd0)
      else $fatal(1, "worse-than-depth Add was not ignored");

    // Missing Cancel and Trade events must not modify an existing level.
    reset_dut();
    submit_event('{64'd31, EVENT_ADD, SIDE_BID, 32'sd100, 32'd10});
    submit_event('{64'd32, EVENT_CANCEL, SIDE_BID, 32'sd999, 32'd1});
    assert (event_error == BOOK_ERROR_MISSING_PRICE)
      else $fatal(1, "missing Cancel did not report an error");
    submit_event('{64'd33, EVENT_TRADE, SIDE_BID, 32'sd999, 32'd1});
    assert (event_error == BOOK_ERROR_MISSING_PRICE)
      else $fatal(1, "missing Trade did not report an error");
    expect_bid(0, 32'sd100, 32'd10);

    $display("Phase 4 order-book tests passed.");
    $finish;
  end
endmodule
