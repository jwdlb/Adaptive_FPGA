`timescale 1ns/1ps

// This adapter gives market_pipeline one stable, compact result register for
// streaming results into C++. It is deliberately not a FIFO: it holds exactly
// one completed result until result_ready accepts it. That prevents the old
// one-cycle event_done pulse from being missed, while leaving the larger backlog
// in the host-side SPSC ring rather than consuming FPGA memory.
module market_stream_adapter #(
  parameter bit ALLOW_CROSSED_BOOKS = 1'b0
) (
  input  logic                            clk,
  input  logic                            rst_n,
  input  logic                            in_valid,
  output logic                            in_ready,
  input  market_types_pkg::market_event_t in_event,
  output logic                            event_done,
  output market_types_pkg::book_error_t   event_error,
  output logic signed [63:0]              order_flow_delta,
  output logic signed [63:0]              trade_flow_delta,
  output market_types_pkg::price_level_t  bid_snapshot [0:market_types_pkg::BOOK_DEPTH-1],
  output market_types_pkg::price_level_t  ask_snapshot [0:market_types_pkg::BOOK_DEPTH-1],
  output logic signed [31:0]              feature_values [0:market_types_pkg::FEATURE_COUNT-1],
  output logic                            feature_valid,
  output logic signed [31:0]              score,
  output market_types_pkg::action_t       action,
  output logic                            signal_valid,
  output logic        [63:0]              signal_timestamp_ns,
  output logic        [63:0]              signal_event_index,
  output logic        [63:0]              model_version,
  output logic        [63:0]              update_count,
  input  logic                            param_write_valid,
  input  logic        [3:0]               param_write_index,
  input  logic signed [31:0]              param_write_value,
  input  logic        [63:0]              param_write_model_version,
  input  logic                            param_commit,

  // Compact, held result for the future RTL-worker-to-SPSC hand-off.
  input  logic                            result_ready,
  output logic                            result_valid,
  output logic        [63:0]              result_event_index,
  output logic        [63:0]              result_timestamp_ns,
  output market_types_pkg::book_error_t   result_error,
  output logic signed [31:0]              result_feature_values [0:market_types_pkg::FEATURE_COUNT-1],
  output logic                            result_feature_valid
);
  import market_types_pkg::*;

  logic core_in_ready;
  logic core_in_valid;
  logic core_event_done;
  logic [63:0] accepted_timestamp_ns;
  logic [63:0] accepted_event_index;
  logic [63:0] next_event_index;

  // Do not allow another event to enter while the one-result register is still
  // occupied. This guarantees a completed result can never be overwritten.
  assign in_ready = core_in_ready && !result_valid;
  assign core_in_valid = in_valid && in_ready;
  assign event_done = core_event_done;

  // The original pipeline keeps ownership of the market book, feature engine,
  // strategy model, and parameter shadow-bank behaviour.
  market_pipeline #(.ALLOW_CROSSED_BOOKS(ALLOW_CROSSED_BOOKS)) core (
    .clk, .rst_n, .in_valid(core_in_valid), .in_ready(core_in_ready), .in_event,
    .event_done(core_event_done), .event_error, .order_flow_delta, .trade_flow_delta,
    .bid_snapshot, .ask_snapshot, .feature_values, .feature_valid, .score, .action,
    .signal_valid, .signal_timestamp_ns, .signal_event_index, .model_version, .update_count,
    .param_write_valid, .param_write_index, .param_write_value,
    .param_write_model_version, .param_commit
  );

  // Remember the metadata as each input handshake occurs. The core's current
  // signal event index deliberately counts valid signals only, whereas this
  // stream index identifies every input event, including one that returns an
  // order-book error.
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      accepted_timestamp_ns <= '0;
      accepted_event_index <= '0;
      next_event_index <= '0;
    end else if (core_in_valid && core_in_ready) begin
      accepted_timestamp_ns <= in_event.timestamp_ns;
      accepted_event_index <= next_event_index;
      next_event_index <= next_event_index + 64'd1;
    end
  end

  // Capture one complete compact result when the core finishes. Once valid is
  // high, every captured field remains unchanged until C++ raises result_ready.
  // result_ready and core_event_done cannot normally coincide because input is
  // gated while valid; giving a new completion priority is nevertheless safe.
  always_ff @(posedge clk) begin
    if (!rst_n) begin
      result_valid <= 1'b0;
      result_event_index <= '0;
      result_timestamp_ns <= '0;
      result_error <= BOOK_ERROR_NONE;
      result_feature_valid <= 1'b0;
      for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin
        result_feature_values[index] <= '0;
      end
    end else begin
      if (result_valid && result_ready) result_valid <= 1'b0;
      if (core_event_done) begin
        result_valid <= 1'b1;
        result_event_index <= accepted_event_index;
        result_timestamp_ns <= accepted_timestamp_ns;
        result_error <= event_error;
        result_feature_valid <= feature_valid;
        for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin
          result_feature_values[index] <= feature_values[index];
        end
      end
    end
  end
endmodule
