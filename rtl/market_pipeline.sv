// Phase 5 top level: order book -> rolling window -> features -> strategy.
module market_pipeline #(
  parameter bit ALLOW_CROSSED_BOOKS = 1'b0
) (
  // Normal clock and reset signals.
  input  logic                            clk,
  input  logic                            rst_n,
  // This is the normal valid/ready handshake
  input  logic                            in_valid,
  output logic                            in_ready,
  input  market_types_pkg::market_event_t in_event,
  // These report the result of processing one event
  output logic                            event_done,
  output market_types_pkg::book_error_t   event_error,
  output logic signed [63:0]              order_flow_delta,
  output logic signed [63:0]              trade_flow_delta,
  // These expose the current bid and ask order-book levels for simulation/debugging
  output market_types_pkg::price_level_t  bid_snapshot [0:market_types_pkg::BOOK_DEPTH-1],
  output market_types_pkg::price_level_t  ask_snapshot [0:market_types_pkg::BOOK_DEPTH-1],
  // Feature stuff
  output logic signed [31:0]              feature_values [0:market_types_pkg::FEATURE_COUNT-1],  // feature vector of eight Q16.16 values 
  output logic                            feature_valid, // both book sides exist, so features make sense
  output logic signed [31:0]              score, // weighted model score
  output market_types_pkg::action_t       action,  // ACTION_BUY, ACTION_HOLD, or ACTION_SELL
  output logic                            signal_valid,  // strategy received valid features
  // These describe where the signal came from
  output logic        [63:0]              signal_timestamp_ns,
  output logic        [63:0]              signal_event_index,
  output logic        [63:0]              model_version,
  output logic        [63:0]              update_count,
  // This lets another component, i.e. the GPU-learning path, write a new model into the strategy model’s shadow bank
  input  logic                            param_write_valid,
  input  logic        [3:0]               param_write_index,
  input  logic signed [31:0]              param_write_value,
  input  logic        [63:0]              param_write_model_version,
  input  logic                            param_commit
);
  // Imports the shared types and Q16.16 math helpers.
  import market_types_pkg::*;
  import fixed_point_pkg::*;

  // This pipeline has three states:
  // - WAIT_BOOK    = wait for the order book to finish an event
  // - EMIT_FEATURE = emit the calculated feature/signal result
  // - EMIT_ERROR   = report a failed event
  typedef enum logic [1:0] {WAIT_BOOK, EMIT_FEATURE, EMIT_ERROR} pipeline_state_t;
  pipeline_state_t pipeline_state;

  // These connect to the internal order_book module. They are called core_... to distinguish them from the final pipeline outputs
  logic core_in_ready;
  logic core_event_done;
  book_error_t core_event_error;
  logic signed [63:0] core_order_flow_delta;
  logic signed [63:0] core_trade_flow_delta;
  price_level_t core_bids [0:BOOK_DEPTH-1];
  price_level_t core_asks [0:BOOK_DEPTH-1];
  // This becomes 1 when the order book has finished processing an event and the pipeline is ready to push the new data into the rolling window.
  logic window_push;

  // These allow us to calculate price movement
  logic signed [63:0] current_midpoint;
  logic current_midpoint_valid;
  logic signed [63:0] prior_midpoint;
  logic prior_midpoint_valid;
  logic signed [63:0] midpoint_change;

  // These come from feature_window.sv, they represent the rolling sums of the last N events, where N is the window size.
  logic signed [63:0] window_order_flow_sum;
  logic signed [63:0] window_trade_flow_sum;
  logic signed [63:0] window_midpoint_change_sum;
  logic [6:0] window_size;

  // The pipeline remembers the timestamp of the event it accepted, then attaches it to the final signal
  logic [63:0] accepted_timestamp_ns;
  logic [63:0] next_event_index;

  // These are the internal signals that control the strategy model. They are derived from the pipeline state and the param_commit input.
  logic strategy_evaluate;
  logic strategy_param_commit;

  // Creates an instance of order_book.sv, named book
  // - #(.ALLOW_CROSSED_BOOKS(ALLOW_CROSSED_BOOKS)) Passes this pipeline’s parameter into the order-book parameter
  // - .clk, .rst_n, .in_valid, .in_ready(core_in_ready), .in_event, Connects the pipeline’s clock, reset, and input signals to the order book
  // - .event_done(core_event_done), .event_error(core_event_error), Connects the order book’s outputs to the pipeline’s internal signals
  // - .order_flow_delta(core_order_flow_delta), .trade_flow_delta(core_trade_flow_delta), Connects the order book’s outputs to the pipeline’s internal signals
  // - .bid_snapshot(core_bids), .ask_snapshot(core_asks) Connects the order book’s outputs to the pipeline’s internal signals
  // shorthand .clk means .clk(clk), and the outputs are done in here through the .x(y) syntax, y is intrenal signal here and x is the output of the module.
  order_book #(.ALLOW_CROSSED_BOOKS(ALLOW_CROSSED_BOOKS)) book (
    .clk, .rst_n, .in_valid, .in_ready(core_in_ready), .in_event,
    .event_done(core_event_done), .event_error(core_event_error),
    .order_flow_delta(core_order_flow_delta), .trade_flow_delta(core_trade_flow_delta),
    .bid_snapshot(core_bids), .ask_snapshot(core_asks)
  );

  always_comb begin  // This is combinational logic, therefore it recalculates whenever the order-book snapshots change.
    current_midpoint_valid = core_bids[0].quantity != 0 && core_asks[0].quantity != 0;   // The midpoint only exists if both best levels exist.
    current_midpoint = (i32_to_i64(core_bids[0].price_ticks) + i32_to_i64(core_asks[0].price_ticks)) / 64'sd2;  // Calculates: (best bid price + best ask price) / 2, i32_to_i64() safely widens a signed 32-bit price into 64 bits before addition.
    
    // Only calculate movement if both the previous and current midpoints exist.
    if (prior_midpoint_valid && current_midpoint_valid) begin
      // This calculates the absolute difference
      midpoint_change = current_midpoint >= prior_midpoint ?
          current_midpoint - prior_midpoint : prior_midpoint - current_midpoint;
    end else begin
      midpoint_change = '0;   // For the first valid midpoint, or while either side is missing, there is no earlier midpoint to compare against, so movement is zero.
    end
  end

  // The whole pipeline is ready only when the order book is ready AND the pipeline is waiting for a book event
  // This stops a new event entering while the feature/signal result of the previous event is still being emitted.
  assign in_ready = core_in_ready && pipeline_state == WAIT_BOOK;

  // Push a new entry into the rolling window only when: the order book finished AND the event was successful.
  // Failed events do not affect rolling sums.
  assign window_push = pipeline_state == WAIT_BOOK && core_event_done && core_event_error == BOOK_ERROR_NONE;

  // Tell the strategy model to calculate its score when the pipeline reaches EMIT_FEATURE
  assign strategy_evaluate = pipeline_state == EMIT_FEATURE;

  // A parameter commit is only allowed when the pipeline/order book are idle.
  // That is what makes parameter updates atomic with respect to events: a model does not change halfway through processing an event.
  assign strategy_param_commit = param_commit && pipeline_state == WAIT_BOOK && core_in_ready;

  // Creates the 64-event rolling-window module.
  // - On every successful event, it stores: order flow delta, trade flow delta, and absolute midpoint change
  // - It outputs the sums and current number of stored events.
  feature_window window (
    .clk, .rst_n, .push_valid(window_push),
    .order_flow(core_order_flow_delta), .trade_flow(core_trade_flow_delta), .midpoint_change,
    .order_flow_sum(window_order_flow_sum), .trade_flow_sum(window_trade_flow_sum),
    .midpoint_change_sum(window_midpoint_change_sum), .size(window_size)
  );

  // Creates the module that calculates the eight Q16.16 features.
  feature_engine features (
    .bids(core_bids), .asks(core_asks),
    .order_flow_sum(window_order_flow_sum), .trade_flow_sum(window_trade_flow_sum),
    .midpoint_change_sum(window_midpoint_change_sum), .window_size,
    .feature_values, .feature_valid
  );

  // Creates the parameter-bank and model-scoring module. 
  // When strategy_evaluate is high, it uses the current feature values and validity flag to calculate:
  // - score = weighted sum of the eight features, using the current model parameters
  // - action = BUY, HOLD, or SELL, based on the score and the current
  // - signal_valid = whether the features were valid, i.e. both book sides existed
  strategy_model strategy (
    .clk, .rst_n, .evaluate(strategy_evaluate), .feature_values, .feature_valid,
    .param_write_valid, .param_write_index, .param_write_value,
    .param_write_model_version, .param_commit(strategy_param_commit),
    .score, .action, .signal_valid, .model_version, .update_count
  );

  // Copies the internal order-book snapshots to the pipeline’s external outputs.
  always_comb begin
    for (int unsigned index = 0; index < BOOK_DEPTH; index++) begin
      bid_snapshot[index] = core_bids[index];
      ask_snapshot[index] = core_asks[index];
    end
    // Also forwards the book’s flow deltas to the outside world
    order_flow_delta = core_order_flow_delta;
    trade_flow_delta = core_trade_flow_delta;
  end
  // This is the clocked control logic, activating on the rising edge of the clock. It implements the pipeline state machine.
  always_ff @(posedge clk) begin
    // When active-low reset is asserted, clear pipeline state.
    if (!rst_n) begin
      pipeline_state <= WAIT_BOOK;
      event_done <= 1'b0;
      event_error <= BOOK_ERROR_NONE;
      prior_midpoint <= '0;
      prior_midpoint_valid <= 1'b0;
      accepted_timestamp_ns <= '0;
      signal_timestamp_ns <= '0;
      signal_event_index <= '0;
      next_event_index <= '0;
    end else begin
      event_done <= 1'b0;  // Makes event_done a one-cycle pulse. A later state sets it to 1; on the next clock it returns to 0
      if (in_valid && in_ready) accepted_timestamp_ns <= in_event.timestamp_ns;  // When an event enters the pipeline, save its timestamp. The later feature/signal output needs to identify which event created it.

      unique case (pipeline_state)

        // Wait for the internal order book to finish.
        WAIT_BOOK: if (core_event_done) begin
          // Forward the book’s error to the pipeline output.
          event_error <= core_event_error;
          // If the event succeeded:
          if (core_event_error == BOOK_ERROR_NONE) begin
            // Save the current midpoint, ready for comparison with the next successful event.
            if (current_midpoint_valid) begin
              prior_midpoint <= current_midpoint;
              prior_midpoint_valid <= 1'b1;
            end
            pipeline_state <= EMIT_FEATURE; // Move to the state that produces the signal.
          end else begin
            pipeline_state <= EMIT_ERROR; // If the book event failed No valid feature/signal should be produced for that failed event.
          end
        end

        // Signal producing state: the pipeline has a new feature/signal to emit. It also attaches the timestamp and sequential event index to the signal.
        EMIT_FEATURE: begin
          // Attach metadata to this newly produced signal, then advance the event index.
          signal_timestamp_ns <= accepted_timestamp_ns;
          signal_event_index <= next_event_index;
          next_event_index <= next_event_index + 64'd1;
          // Pulse completion and return to the state that can accept another event.
          event_done <= 1'b1;
          pipeline_state <= WAIT_BOOK;
        end

        // Even a failed event must finish cleanly. This reports completion, keeps the error code, and returns to idle.
        EMIT_ERROR: begin
          event_done <= 1'b1;
          pipeline_state <= WAIT_BOOK;
        end
        
        // Safety fallback: if the FSM somehow gets into an unknown state, report an invariant error and recover to WAIT_BOOK.
        default: begin
          event_error <= BOOK_ERROR_INVARIANT_VIOLATION;
          event_done <= 1'b1;
          pipeline_state <= WAIT_BOOK;
        end
      endcase
    end
  end
endmodule
