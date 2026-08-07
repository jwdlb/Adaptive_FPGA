`timescale 1ns/1ps

// C++-friendly wrapper around the full RTL pipeline. It converts flat scalar C++
// inputs into the packed RTL event and parameter records, then exposes snapshots,
// features, signal values, and parameter-bank state as scalar outputs.
module market_pipeline_cpp_wrapper (
  input logic clk,
  input logic rst_n,
  input logic in_valid,
  output logic in_ready,
  input logic [63:0] timestamp_ns,
  input logic [1:0] event_type,
  input logic side,
  input logic signed [31:0] price_ticks,
  input logic [31:0] quantity,
  output logic event_done,
  output market_types_pkg::book_error_t event_error,
  output logic signed [63:0] order_flow_delta,
  output logic signed [63:0] trade_flow_delta,
  output logic [63:0] bid_level0, output logic [63:0] bid_level1,
  output logic [63:0] bid_level2, output logic [63:0] bid_level3,
  output logic [63:0] bid_level4, output logic [63:0] bid_level5,
  output logic [63:0] bid_level6, output logic [63:0] bid_level7,
  output logic [63:0] bid_level8, output logic [63:0] bid_level9,
  output logic [63:0] ask_level0, output logic [63:0] ask_level1,
  output logic [63:0] ask_level2, output logic [63:0] ask_level3,
  output logic [63:0] ask_level4, output logic [63:0] ask_level5,
  output logic [63:0] ask_level6, output logic [63:0] ask_level7,
  output logic [63:0] ask_level8, output logic [63:0] ask_level9,
  output logic signed [31:0] feature0, output logic signed [31:0] feature1,
  output logic signed [31:0] feature2, output logic signed [31:0] feature3,
  output logic signed [31:0] feature4, output logic signed [31:0] feature5,
  output logic signed [31:0] feature6, output logic signed [31:0] feature7,
  output logic feature_valid,
  output logic signed [31:0] score,
  output market_types_pkg::action_t action,
  output logic signal_valid,
  output logic [63:0] signal_timestamp_ns,
  output logic [63:0] signal_event_index,
  output logic [63:0] model_version,
  output logic [63:0] update_count,
  input logic param_write_valid,
  input logic [3:0] param_write_index,
  input logic signed [31:0] param_write_value,
  input logic [63:0] param_write_model_version,
  input logic param_commit
);
  import market_types_pkg::*;

  market_event_t in_event;
  price_level_t bid_snapshot [0:BOOK_DEPTH-1];
  price_level_t ask_snapshot [0:BOOK_DEPTH-1];
  logic signed [31:0] feature_values [0:FEATURE_COUNT-1];

  always_comb begin
    in_event = '{timestamp_ns: timestamp_ns, event_type: event_type_t'(event_type),
                 side: side_t'(side), price_ticks: price_ticks, quantity: quantity};
    bid_level0 = bid_snapshot[0]; bid_level1 = bid_snapshot[1]; bid_level2 = bid_snapshot[2]; bid_level3 = bid_snapshot[3]; bid_level4 = bid_snapshot[4];
    bid_level5 = bid_snapshot[5]; bid_level6 = bid_snapshot[6]; bid_level7 = bid_snapshot[7]; bid_level8 = bid_snapshot[8]; bid_level9 = bid_snapshot[9];
    ask_level0 = ask_snapshot[0]; ask_level1 = ask_snapshot[1]; ask_level2 = ask_snapshot[2]; ask_level3 = ask_snapshot[3]; ask_level4 = ask_snapshot[4];
    ask_level5 = ask_snapshot[5]; ask_level6 = ask_snapshot[6]; ask_level7 = ask_snapshot[7]; ask_level8 = ask_snapshot[8]; ask_level9 = ask_snapshot[9];
    feature0 = feature_values[0]; feature1 = feature_values[1]; feature2 = feature_values[2]; feature3 = feature_values[3];
    feature4 = feature_values[4]; feature5 = feature_values[5]; feature6 = feature_values[6]; feature7 = feature_values[7];
  end

  // The real market pipeline remains responsible for all market, feature, signal,
  // and atomic parameter-bank behaviour.
  market_pipeline dut (.*);
endmodule
