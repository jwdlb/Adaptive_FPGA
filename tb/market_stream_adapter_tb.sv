`timescale 1ns/1ps

// Verilog-only proof for the one-result streaming adapter. It deliberately
// holds result_ready low for several clocks and checks that every compact result
// field remains unchanged and that the core refuses a new input meanwhile.
module market_stream_adapter_tb;
  import market_types_pkg::*;

  logic clk;
  logic rst_n = 1'b0;
  logic in_valid = 1'b0;
  logic in_ready;
  market_event_t in_event;
  /* verilator lint_off UNUSEDSIGNAL */
  logic event_done;
  book_error_t event_error;
  logic signed [63:0] order_flow_delta;
  logic signed [63:0] trade_flow_delta;
  price_level_t bid_snapshot [0:BOOK_DEPTH-1];
  price_level_t ask_snapshot [0:BOOK_DEPTH-1];
  logic signed [31:0] feature_values [0:FEATURE_COUNT-1];
  logic feature_valid;
  logic signed [31:0] score;
  action_t action;
  logic signal_valid;
  logic [63:0] signal_timestamp_ns;
  logic [63:0] signal_event_index;
  logic [63:0] model_version;
  logic [63:0] update_count;
  /* verilator lint_on UNUSEDSIGNAL */
  logic param_write_valid = 1'b0;
  logic [3:0] param_write_index = '0;
  logic signed [31:0] param_write_value = '0;
  logic [63:0] param_write_model_version = '0;
  logic param_commit = 1'b0;
  logic result_ready = 1'b0;
  logic result_valid;
  logic [63:0] result_event_index;
  logic [63:0] result_timestamp_ns;
  book_error_t result_error;
  logic signed [31:0] result_feature_values [0:FEATURE_COUNT-1];
  logic result_feature_valid;
  /* verilator lint_off UNUSEDSIGNAL */
  logic signed [31:0] result_best_bid_price_ticks;
  logic [31:0] result_best_bid_quantity;
  logic signed [31:0] result_best_ask_price_ticks;
  logic [31:0] result_best_ask_quantity;
  /* verilator lint_on UNUSEDSIGNAL */
  logic signed [31:0] held_feature_values [0:FEATURE_COUNT-1];
  logic held_feature_valid;

  market_stream_adapter dut (.*);

  always #5 clk = ~clk;

  task automatic send_event(
      input logic [63:0] timestamp,
      input event_type_t event_type,
      input side_t side,
      input logic signed [31:0] price,
      input logic [31:0] quantity
  );
    begin
      @(negedge clk);
      while (!in_ready) @(negedge clk);
      in_event = '{timestamp_ns: timestamp, event_type: event_type, side: side,
                   price_ticks: price, quantity: quantity};
      in_valid = 1'b1;
      @(negedge clk);
      in_valid = 1'b0;
    end
  endtask

  task automatic wait_for_held_result;
    begin
      while (!result_valid) @(negedge clk);
    end
  endtask

  initial begin
    clk = 1'b0;
    in_event = '0;
    repeat (2) @(negedge clk);
    rst_n = 1'b1;
    @(negedge clk);
    if (!in_ready) $fatal(1, "adapter did not become ready after reset");

    // A result from the first event must remain stable while C++ says it has no
    // room in its SPSC ring by leaving result_ready low.
    send_event(64'd1000, EVENT_ADD, SIDE_BID, 32'sd100, 32'd10);
    wait_for_held_result();
    if (result_event_index != 64'd0 || result_timestamp_ns != 64'd1000 ||
        result_error != BOOK_ERROR_NONE) begin
      $fatal(1, "first held result metadata is incorrect");
    end
    held_feature_valid = result_feature_valid;
    for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin
      held_feature_values[index] = result_feature_values[index];
    end
    if (in_ready) $fatal(1, "adapter accepted a new input while result is held");

    repeat (3) begin
      @(negedge clk);
      if (!result_valid || result_event_index != 64'd0 ||
          result_timestamp_ns != 64'd1000 || result_error != BOOK_ERROR_NONE) begin
        $fatal(1, "held result changed before ready handshake");
      end
      if (result_feature_valid != held_feature_valid) begin
        $fatal(1, "held result feature-valid bit changed before ready handshake");
      end
      for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin
        if (result_feature_values[index] != held_feature_values[index]) begin
          $fatal(1, "held result feature changed before ready handshake");
        end
      end
      if (in_ready) $fatal(1, "input became ready before held result was accepted");
    end

    // One ready clock consumes the held result and makes the core input ready
    // again. The next event must receive the next sequential stream index.
    result_ready = 1'b1;
    @(negedge clk);
    if (result_valid) $fatal(1, "result did not clear after ready handshake");
    if (!in_ready) $fatal(1, "input did not reopen after result handshake");
    result_ready = 1'b0;

    send_event(64'd2000, EVENT_ADD, SIDE_ASK, 32'sd110, 32'd10);
    wait_for_held_result();
    if (result_event_index != 64'd1 || result_timestamp_ns != 64'd2000 ||
        result_error != BOOK_ERROR_NONE) begin
      $fatal(1, "second held result metadata is incorrect");
    end

    $display("market_stream_adapter_tb PASSED");
    $finish;
  end
endmodule
