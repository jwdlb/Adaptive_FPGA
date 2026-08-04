`timescale 1ns/1ps
module feature_window_tb;
  logic clk = 1'b0;
  logic rst_n = 1'b0;
  logic push_valid;
  logic signed [63:0] order_flow;
  logic signed [63:0] trade_flow;
  logic signed [63:0] midpoint_change;
  logic signed [63:0] order_flow_sum;
  logic signed [63:0] trade_flow_sum;
  logic signed [63:0] midpoint_change_sum;
  logic [6:0] size;

  always #5 clk = ~clk;
  feature_window dut (.*);

  task automatic push(input logic signed [63:0] value);
    @(negedge clk);
    push_valid = 1'b1;
    order_flow = value;
    trade_flow = -value;
    midpoint_change = value;
    @(negedge clk);
    push_valid = 1'b0;
  endtask

  initial begin
    push_valid = 1'b0;
    order_flow = '0;
    trade_flow = '0;
    midpoint_change = '0;
    repeat (2) @(posedge clk);
    rst_n = 1'b1;
    for (int signed value = 1; value <= 64; value++) push(value);
    assert (size == 7'd64 && order_flow_sum == 64'sd2080 && trade_flow_sum == -64'sd2080);
    push(65);
    assert (size == 7'd64 && order_flow_sum == 64'sd2144 && trade_flow_sum == -64'sd2144 && midpoint_change_sum == 64'sd2144);
    $display("feature_window tests passed.");
    $finish;
  end
endmodule
