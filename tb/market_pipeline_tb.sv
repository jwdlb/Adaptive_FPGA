`timescale 1ns/1ps   // means simulation time uses nanoseconds, with picosecond precision.
module market_pipeline_tb;
  import market_types_pkg::*;
  import fixed_point_pkg::*;

  // Creates a clock starting at 0 and an active-low reset starting active.
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  
  // These are the event input handshake signals.
  logic in_valid;  // Testbench sets this high to indicate a new event is available.
  market_event_t in_event;    // Testbench sets this to the new event when in_valid is high.
  logic in_ready;  // Pipeline sets this high to indicate it is ready to accept a new event.

  // These are the pipeline’s result outputs after an event finishes.
  logic event_done;
  book_error_t event_error;
  logic signed [63:0] order_flow_delta;
  logic signed [63:0] trade_flow_delta;

  // These let the testbench inspect the current book levels.
  price_level_t bid_snapshot [0:BOOK_DEPTH-1];
  price_level_t ask_snapshot [0:BOOK_DEPTH-1];

  // These are the eight calculated Q16.16 features and the signal saying whether they are meaningful.
  logic signed [31:0] feature_values [0:FEATURE_COUNT-1];
  logic feature_valid;

  // These are the strategy-model outputs:
  logic signed [31:0] score;  // = final Q16.16 model score
  action_t action;  // = BUY, HOLD, or SELL
  logic signal_valid;  // = whether the model received valid features

  // These give metadata about the signal and parameter bank:
  logic [63:0] signal_timestamp_ns;  // = timestamp of the event that produced the signal
  logic [63:0] signal_event_index;  // = sequential successful-event number
  logic [63:0] model_version;  // = active model parameter version
  logic [63:0] update_count;  // = number of successful parameter-bank commits

  // These are the parameter-writing interface.
  logic param_write_valid;
  logic [3:0] param_write_index;
  logic signed [31:0] param_write_value;
  logic [63:0] param_write_model_version;
  logic param_commit;

  // Toggles the clock every 5 ns: 0 ns   clk = 0, 5 ns   clk = 1, 10 ns clk = 0, 15 ns clk = 1, ...
  always #5 clk = ~clk;

  // Creates the real hardware module being tested.
  // - dut means “Device Under Test.”
  // - .* automatically connects same-named testbench signals to same-named module ports. For example: testbench.clk -> dut.clk, testbench.in_event -> dut.in_event, dut.score -> testbench.score
  market_pipeline dut (.*);

  // This is a reusable mini-procedure for writing one model parameter into the pipeline’s shadow bank.
  task automatic write_parameter(input logic [3:0] index, input logic signed [31:0] value);
    while (!in_ready) @(posedge clk);
    @(negedge clk);
    param_write_index = index;
    param_write_value = value;
    param_write_model_version = 64'd1;
    param_write_valid = 1'b1;
    @(negedge clk);
    param_write_valid = 1'b0;
  endtask

  task automatic commit_parameters;
    @(negedge clk);
    param_commit = 1'b1;
    @(negedge clk);
    param_commit = 1'b0;
  endtask

  task automatic submit_event(input market_event_t market_event);
    while (!in_ready) @(posedge clk);
    @(negedge clk);
    in_event = market_event;
    in_valid = 1'b1;
    @(negedge clk);
    in_valid = 1'b0;
    while (!event_done) @(posedge clk);
    assert (event_error == BOOK_ERROR_NONE) else $fatal(1, "pipeline event failed");
  endtask

  initial begin
    in_valid = 1'b0;
    in_event = '0;
    param_write_valid = 1'b0;
    param_write_index = '0;
    param_write_value = '0;
    param_write_model_version = '0;
    param_commit = 1'b0;
    repeat (2) @(posedge clk);
    rst_n = 1'b1;

    // The bias-only model always scores +1.0 and therefore buys above zero.
    for (int unsigned index = 0; index < 8; index++) begin
      write_parameter(index[3:0], index == 7 ? Q16_ONE : 32'sd0);
    end
    write_parameter(4'd8, 32'sd0);
    write_parameter(4'd9, -Q16_ONE);
    commit_parameters();
    assert (model_version == 64'd1 && update_count == 64'd1) else $fatal(1, "parameter commit failed");

    // One side is absent: features and the signal must be invalid Hold.
    submit_event('{64'd1, EVENT_ADD, SIDE_BID, 32'sd100, 32'd20});
    assert (!feature_valid && !signal_valid && action == ACTION_HOLD) else $fatal(1, "invalid-book signal is wrong");

    // With both sides present, spread and bias are valid and the bias model buys.
    submit_event('{64'd2, EVENT_ADD, SIDE_ASK, 32'sd102, 32'd10});
    assert (feature_valid && signal_valid && action == ACTION_BUY) else $fatal(1, "valid-book signal is wrong");
    assert (feature_values[0] == 32'sd1298 && feature_values[1] == 32'sd21845 && feature_values[7] == Q16_ONE)
      else $fatal(1, "feature values do not match the reference formulas");
    assert (score == Q16_ONE && signal_timestamp_ns == 64'd2 && signal_event_index == 64'd1)
      else $fatal(1, "signal metadata is wrong");

    $display("market_pipeline tests passed.");
    $finish;
  end
endmodule
