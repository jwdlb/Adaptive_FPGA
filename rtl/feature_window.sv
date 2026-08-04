// Synthesizable 64-event rolling sums used by the flow and volatility features.
module feature_window (
  // Normal clock and active-low reset
  input  logic               clk,
  input  logic               rst_n,
  // push_valid means:  “Store this event’s data into the rolling window now.” . It comes from market_pipeline.sv after a successful order-book event
  input  logic               push_valid,
  // These are the three measurements for the newest event. They are signed 64-bit values because flow can be positive or negative, and totals may grow beyond 32 bits.
  input  logic signed [63:0] order_flow,  // = bid/ask liquidity change
  input  logic signed [63:0] trade_flow,  // = signed quantity traded
  input  logic signed [63:0] midpoint_change,  // = absolute price movement since prior midpoint
  // These are the totals across the latest 64 events.
  output logic signed [63:0] order_flow_sum,
  output logic signed [63:0] trade_flow_sum,
  output logic signed [63:0] midpoint_change_sum,
  // size tells us how many entries are currently stored, it needs 7 bits to represent values 0 through 64.
  output logic        [6:0]  size
);
  import market_types_pkg::*;   // Imports FEATURE_WINDOW_SIZE, which is defined as 64

  // These are the internal 64-entry memories. Each index stores all three measurements for one past event.
  logic signed [63:0] order_flow_entries [0:FEATURE_WINDOW_SIZE-1];
  logic signed [63:0] trade_flow_entries [0:FEATURE_WINDOW_SIZE-1];
  logic signed [63:0] midpoint_entries   [0:FEATURE_WINDOW_SIZE-1];
  // This points to the slot where the next event will be written. It is 6 bits wide because it only needs to represent values 0 through 63
  logic [5:0] next_index;

  always_ff @(posedge clk) begin  // This module updates its stored data only on rising clock edges.
    // If active-low reset is asserted, clear everything.
    if (!rst_n) begin
      next_index          <= '0;
      size                <= '0;
      order_flow_sum      <= '0;
      trade_flow_sum      <= '0;
      midpoint_change_sum <= '0;
      for (int unsigned index = 0; index < FEATURE_WINDOW_SIZE; index++) begin
        order_flow_entries[index] <= '0;
        trade_flow_entries[index] <= '0;
        midpoint_entries[index]   <= '0;
      end
    // If reset is inactive and the pipeline says there is a valid successful event, update the window.
    // If push_valid is 0, nothing changes. The old sums and stored events remain.
    end else if (push_valid) begin
      if (size == 7'd64) begin  // If the window is already full, we need to subtract the oldest event’s values before adding the new event’s values.
        // This means: old total − the oldest stored order flow + the newest event’s order flow
        // next_index points to the oldest entry once the ring is full, because that is the slot about to be overwritten.
        order_flow_sum      <= order_flow_sum - order_flow_entries[next_index] + order_flow;
        trade_flow_sum      <= trade_flow_sum - trade_flow_entries[next_index] + trade_flow;
        midpoint_change_sum <= midpoint_change_sum - midpoint_entries[next_index] + midpoint_change;
      end else begin  // If fewer than 64 entries exist, there is no old entry to remove. Just add the new values to their totals.
        size                <= size + 7'd1;
        order_flow_sum      <= order_flow_sum + order_flow;
        trade_flow_sum      <= trade_flow_sum + trade_flow;
        midpoint_change_sum <= midpoint_change_sum + midpoint_change;
      end
      // Write the new event’s three measurements into the current ring-buffer slot.
      order_flow_entries[next_index] <= order_flow;
      trade_flow_entries[next_index] <= trade_flow;
      midpoint_entries[next_index]   <= midpoint_change;
      // Move the write position forward for the next event. Once it reaches 63, it wraps to 0 automatically because next_index is only 6 bits wide.
      next_index <= next_index + 6'd1;
    end
  end
endmodule
