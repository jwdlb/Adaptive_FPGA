// Phase 4 starter implementation.
//
// This is the FPGA/RTL version of the project's C++ order book. It stores up
// to ten bid levels (highest price first) and ten ask levels (lowest price
// first), then processes one market event at a time. An event is accepted when
// in_valid and in_ready are high; event_done reports that processing has
// finished, while event_error and the flow-delta outputs report the result.
// The state machine spreads validation, price search, book updates, and
// crossed-book checks across clock cycles. 

// This file is intentionally a skeleton: implement each TODO and compare its behaviour with the C++
// OrderBook reference before moving to the next operation.


// This starts a hardware module called order_book
// #(...) contains module parameters: compile-time configuration values.
module order_book #(
  parameter bit ALLOW_CROSSED_BOOKS = 1'b0    // If 1, allow best bid >= best ask; if 0, reject such events. (default to 0)
) (

  // These are the fundamental control inputs:
  input  logic clk,    // The clock, the main state and order-book storage update on its rising edge.
  input  logic rst_n,  // active-low reset, _n means “negative logic”: reset is active when this signal is 0.

  // A two-signal agreementfor safely sending an event to teh order book.
  input  logic                            in_valid,  // an input to order_book. The sender/testbench drives it high (1) to say: “in_event currently contains a real event I want you to accept.”
  output logic                            in_ready,  // an output from order_book. The order book drives it high (1) to say: “I am free and can accept an event now.” (when teh FSM is IDLE)

  input  market_types_pkg::market_event_t in_event,  // The event data to pocess

  output logic                            event_done,   // Becomes 1 for one clock cycle when processing finishes.
  output market_types_pkg::book_error_t   event_error,  // Tells the caller whether the event failed, for example due to a missing price or crossed book.
  output logic signed [63:0]              order_flow_delta,  // Signed change in visible order quantity, signed so for buy it's positive and ask it's negative. 
  output logic signed [63:0]              trade_flow_delta,  // Signed trade quantity: ask-side trades are positive and bid-side trades are negative.

  // Simulation/debug snapshots. A production synthesis wrapper can omit these
  // ports and access the core state only from a dedicated test wrapper.
  // Ecah is of type price_level_t, so contains a price and quantity. The array is indexed from 0 to BOOK_DEPTH-1, with 0 being the best bid/ask.
  output market_types_pkg::price_level_t bid_snapshot [0:market_types_pkg::BOOK_DEPTH-1],
  output market_types_pkg::price_level_t ask_snapshot [0:market_types_pkg::BOOK_DEPTH-1]
);
  // Imports everything from market_types_pkg into this module’s scope, so no more market_types_pkg::{x}
  import market_types_pkg::*;

  // The order book is implemented as a finite state machine (FSM) with the following states:
  typedef enum logic [3:0] {
    ST_IDLE,  // wait for a new event
    ST_VALIDATE,  // check whether the event is valid
    ST_SEARCH,  // look through the bid or ask levels
    ST_APPLY,  // perform the requested book update
    ST_SHIFT_RIGHT, // shift levels to make room for a new level
    ST_REMOVE,  // remove a level
    ST_CHECK_CROSS,  // reject an invalid crossed book
    ST_COMMIT,  // commit the changes to the book
    ST_FINISH  // report completion, then return to idle
  } state_t;


  state_t state; // The register holding the current FSM state.
  market_event_t current_event;  // Stores the event currently being processed. The incoming in_event may change after the handshake, so the hardware copies it here and works from this stable stored version.
  // These are the actual internal order-book arrays.
  price_level_t bids [0:BOOK_DEPTH-1];  // stored bid-side price levels
  price_level_t asks [0:BOOK_DEPTH-1];  // stored ask-side price levels

  // The live book is only updated after all validation succeeds.
  // This makes crossed-book rejection atomic.
  price_level_t work_bids [0:BOOK_DEPTH-1];
  price_level_t work_asks [0:BOOK_DEPTH-1];

  logic [3:0] shift_index;
  logic       operation_changes_book;

  logic [3:0] scan_index;  // Tracks which price level the search is currently examining.  It is 4 bits wide, which can represent 0 through 15. That is enough to count through levels 0–9 and also reach 10, which means “search finished.”
  logic [3:0] match_index;  // Stores the array index where the event’s price was found.
  logic [3:0] insert_index;  // Stores the place where a new price should be inserted if no matching price exists.
  // One-bit flags to indicate whether a match or insertion point was found during the search.
  logic       match_found;  // match_found = 1: the price already exists in the book.
  logic       insert_found;  // insert_found = 1: the hardware has found where a new price should be inserted.

  function automatic price_level_t selected_work_level(input logic [3:0] index);
    if (current_event.side == SIDE_BID) begin
      return work_bids[index];
    end
    return work_asks[index];
  endfunction

  function automatic logic side_is_bid();
    return current_event.side == SIDE_BID;
  endfunction

  function automatic logic signed [63:0] order_flow_sign();
    return side_is_bid() ? 64'sd1 : -64'sd1;
  endfunction

  // Returns true when the current level appears before current_event's price.
  // This is used only to locate a possible insertion point; the shift/write
  // implementation belongs in ST_APPLY.
  // 1 bit output, 1 for selected level price is before event, 0 for selected level price is not before event. The input is a price_level_t struct.
  function automatic logic price_precedes_event(input price_level_t level);
    if (current_event.side == SIDE_BID) begin
      return level.price_ticks > current_event.price_ticks;
    end
    return level.price_ticks < current_event.price_ticks;
  endfunction

  // This continuously sets in_ready, as assign is combinational logic and does not require a clock edge, it updates whenever state changes. 
  assign in_ready = (state == ST_IDLE);

  // This continually copies the internal book storage to the snapshot outputs. (as the snapshots are outputs, teh internal books aren't directly visible to the testbench)
  // always_comb is a SystemVerilog construct that describes combinational logic, meaning it continuously evaluates the block whenever any of its inputs change. 
  always_comb begin
    for (int unsigned level = 0; level < BOOK_DEPTH; level++) begin
      bid_snapshot[level] = bids[level];
      ask_snapshot[level] = asks[level];
    end
  end
  //Main FSM code block.
  // always_ff @(posedge clk) describes sequential logic that updates on the rising edge of the clock.

  always_ff @(posedge clk) begin
    // Whenevr we want to reset the order book, we set all state and storage to zero. This is a synchronous reset, meaning it happens on the clock edge when rst_n is low.
    if (!rst_n) begin 
      state            <= ST_IDLE;
      current_event    <= '0;
      scan_index       <= '0;
      match_index      <= '0;
      insert_index     <= '0;
      match_found      <= 1'b0;
      insert_found     <= 1'b0;
      event_done       <= 1'b0;
      event_error      <= BOOK_ERROR_NONE;
      order_flow_delta <= '0;
      trade_flow_delta <= '0;
      shift_index            <= '0;
      operation_changes_book <= 1'b0;

      for (int unsigned level = 0; level < BOOK_DEPTH; level++) begin
        bids[level] <= '0;
        asks[level] <= '0;
        work_bids[level] <= '0;
        work_asks[level] <= '0;
      end
    end else begin
      // Default makes event_done a one-cycle pulse in ST_FINISH.
      event_done <= 1'b0;

      unique case (state)  // Unique means that the synthesizer can assume that only one of the cases will be true at a time, which can help to detect unexpected state values in simulation.
        ST_IDLE: begin
          if (in_valid && in_ready) begin  // Accept only on a valid/ready handshake.
            current_event    <= in_event;  // copy current event into internal storage
            // Clears the output result values for this new event. The FSM will update them as it processes the event.
            event_error      <= BOOK_ERROR_NONE;
            order_flow_delta <= '0;
            trade_flow_delta <= '0;
            operation_changes_book <= 1'b0;

            for (int unsigned level = 0; level < BOOK_DEPTH; level++) begin
              work_bids[level] <= bids[level];
              work_asks[level] <= asks[level];
            end
            // Moves to validation on the next clock cycle, where the FSM will check whether the event is well-formed.
            state            <= ST_VALIDATE;
          end
          // If in_valid is zero, nothing changes: the design stays idle and ready.
        end

        ST_VALIDATE: begin
          if (!event_is_well_formed(current_event)) begin  // If the event doesn't follows the protocol defined earlier.
            event_error <= BOOK_ERROR_INVALID_EVENT;
            state       <= ST_FINISH;
          end else begin // If the event does follow the protocol, then we prepare to search for a matching price level or an insertion point.
            // Initialises the search variables: 
            scan_index   <= '0;
            match_index  <= '0;
            insert_index <= BOOK_DEPTH;  // Represents “no insertion point found yet” because the valid indices are 0–9. If we find a valid insertion point, we will overwrite this value with the correct index.
            match_found  <= 1'b0;
            insert_found <= 1'b0;
            // Moves to the multi-clock search state.
            state        <= ST_SEARCH;
          end
        end

        ST_SEARCH: begin
          // What it needs to answer:
          // 1. Is there already a level at this event’s price?
          // 2. If not, where should a new level be inserted to keep prices ordered?

          // Searches at most one level per cycle. It deliberately does not
          // mutate storage; use its recorded indices in ST_APPLY.

          if (scan_index == BOOK_DEPTH) begin  // Once the search has scanned all levels, this is the only way to move over to the next state. The FSM will then apply the event to the book.
            state <= ST_APPLY;  // move to the apply state.
          end else begin  // If there are still levels to scan, then check the current level for a match or insertion point.
            // Checks whether the current level is empty, selected_level(...) chooses whether to look at bids or asks based on the current event's side.
            if (selected_work_level(scan_index).quantity == 0) begin
              // If the current level is empty, then it is a valid insertion point. If we haven't already found an insertion point, record this index as the insertion point.
              if (!insert_found) begin
                insert_index <= scan_index; // set the insertion index to the current scan index
                insert_found <= 1'b1;  // set the insert_found flag to true, so we don't overwrite it with a later empty level
              end
            end else if (selected_work_level(scan_index).price_ticks == current_event.price_ticks) begin  // if the slot is not empty, check whether the price matches the current event's price.
              match_index <= scan_index;  // record the index of the matching price level
              match_found <= 1'b1;  // set the match_found flag to true, so we don't overwrite it with a later match
            end else if (!insert_found && !price_precedes_event(selected_work_level(scan_index))) begin  // if we find an insertion point in the middle of the active levels ( price_precedes_event(level) asks whether the existing level should remain before the incoming event ).
              insert_index <= scan_index;  // record the index of the insertion point
              insert_found <= 1'b1;  // set the insert_found flag to true, so we don't overwrite it with a later insertion point
            end
            scan_index <= scan_index + 1'b1;  // increment the scan index to look at the next level on the next clock cycle
          end
        end

        ST_APPLY: begin
          unique case (current_event.event_type)
            // An add event has two possibilites:
            // Price already exists -> add to its quantity.
            // Price does not exist -> insert a new price level.
            EVENT_ADD: begin
              if (match_found) begin  // If the price already exists
                if (side_is_bid()) begin  // If the event is a BID, work with work_bids
                  order_flow_delta <= order_flow_sign() *   // Calculates the visible quantity that needs to be added. order_flow_sign() returns +1 for bids and -1 for asks, so the result is signed correctly.
                      $signed({1'b0,  
                        saturated_add_u32(
                          work_bids[match_index].quantity,
                          current_event.quantity
                        ) - work_bids[match_index].quantity
                      });

                  work_bids[match_index].quantity <= saturated_add_u32(   // Actually update the candidate bid quantity.
                      work_bids[match_index].quantity,
                      current_event.quantity
                  );
                end else begin  // Else the event is an ASK, work with work_asks
                  order_flow_delta <= order_flow_sign() * // Calculates the visible quantity that needs to be subtracted. order_flow_sign() returns +1 for bids and -1 for asks, so the result is signed correctly.
                      $signed({1'b0,
                        saturated_add_u32(
                          work_asks[match_index].quantity,
                          current_event.quantity
                        ) - work_asks[match_index].quantity
                      });

                  work_asks[match_index].quantity <= saturated_add_u32(  // Actually update the candidate bid quantity.
                      work_asks[match_index].quantity,
                      current_event.quantity
                  );
                end

                // Mark that the candidate book changed, then moves to the crossed-book validation state.
                operation_changes_book <= 1'b1;
                state                  <= ST_CHECK_CROSS;

              end else if (insert_index == BOOK_DEPTH) begin  // If the price doesn't exist and the price is worse than the tenth visible level, then we ignore the event. The book is full and the new price is not good enough to be visible.
                // Worse than the tenth visible level: ignored successfully.
                state <= ST_CHECK_CROSS;

              end else begin  // If the price doesn't exist and the price is within the ten visible levels, then we need to insert a new level. This requires shifting all worse levels down by one to make room for the new level.
                shift_index <= BOOK_DEPTH - 1;
                state       <= ST_SHIFT_RIGHT;
              end
            end

            EVENT_UPDATE: begin
              // An update can only change an existing price level.
              if (!match_found) begin
                event_error <= BOOK_ERROR_MISSING_PRICE;
                state       <= ST_FINISH;

              // An update with zero quantity means: “remove this entire level.”
              end else if (current_event.quantity == 0) begin
                // Update-to-zero removes the complete visible level.
                // - Removing bid liquidity produces negative order flow.
                // - Removing ask liquidity produces positive order flow.
                order_flow_delta <= -order_flow_sign() *
                    $signed({1'b0, selected_work_level(match_index).quantity});

                operation_changes_book <= 1'b1;
                shift_index            <= match_index;
                state                  <= ST_REMOVE;

              // An update withy non-zero auntity means: Replace the old quantity with the new quantity.
              end else begin
                // The flow delta is: new quantity − old quantity
                if (side_is_bid()) begin  // If the event is a BID, work with work_bids
                  order_flow_delta <= order_flow_sign() *
                      ($signed({1'b0, current_event.quantity}) -
                       $signed({1'b0, work_bids[match_index].quantity}));

                  work_bids[match_index].quantity <= current_event.quantity;
                end else begin  // Else the event is an ASK, work with work_asks
                  order_flow_delta <= order_flow_sign() *
                      ($signed({1'b0, current_event.quantity}) -
                       $signed({1'b0, work_asks[match_index].quantity}));  // For an ask order, the quantity difference is multiplied by -1

                  work_asks[match_index].quantity <= current_event.quantity;
                end

                // Then mark the book as changed and validate it.
                operation_changes_book <= 1'b1;
                state                  <= ST_CHECK_CROSS;
              end
            end

            // Both EVENT_TRADE AND EVENT_CANCEL are similar, so they both use the same code block. The difference is:
            // CANCEL -> changes order flow only
            // TRADE  →->changes order flow and trade flow
            EVENT_CANCEL,
            EVENT_TRADE: begin
              // You cannot cancel or trade a price level that does not exist.
              if (!match_found) begin
                event_error <= BOOK_ERROR_MISSING_PRICE;
                state       <= ST_FINISH;

              // Cancel/Trade with zero quantity is invalid.
              end else if (current_event.quantity == 0) begin
                event_error <= BOOK_ERROR_INVALID_EVENT;
                state       <= ST_FINISH;

              // If the requested cancellation/trade is at least as big as the visible quantity, remove all the visible quantity. Then compact the book (i.e. shift all worse levels up by one to fill the empty level). 
              end else if (
                  current_event.quantity >= selected_work_level(match_index).quantity
              ) begin
                // Calculate flow from the quantity actually removed.
                order_flow_delta <= -order_flow_sign() *
                    $signed({1'b0, selected_work_level(match_index).quantity});

                // For a trade, use the same bid-negative / ask-positive sign convention.
                if (current_event.event_type == EVENT_TRADE) begin
                  trade_flow_delta <= -order_flow_sign() *
                      $signed({1'b0, selected_work_level(match_index).quantity});
                end

                operation_changes_book <= 1'b1;
                shift_index            <= match_index;
                state                  <= ST_REMOVE;
                // Start compacting the array to remove the empty level.

              // This branch means the requested quantity is smaller than the visible quantity. No need to compact teh book (i.e. no need to shift levels up or down), just reduce the quantity of the visible level by the requested amount. 
              end else begin
                // Update the quantity of the partciluar level, BID or ASK.
                if (side_is_bid()) begin
                  work_bids[match_index].quantity <=
                      work_bids[match_index].quantity - current_event.quantity;
                end else begin
                  work_asks[match_index].quantity <=
                      work_asks[match_index].quantity - current_event.quantity;
                end

                // The quantity removed is exactly the requested quantity.
                order_flow_delta <= -order_flow_sign() *
                    $signed({1'b0, current_event.quantity});

                // For a trade, use the shared side-sign helper for trade flow too.
                if (current_event.event_type == EVENT_TRADE) begin
                  trade_flow_delta <= -order_flow_sign() *
                      $signed({1'b0, current_event.quantity});
                end

                // Mark the candidate book as changed and checks whether that candidate can be committed.
                operation_changes_book <= 1'b1;
                state                  <= ST_CHECK_CROSS;
              end
            end
            
            // This is a safety fallback. If an unknown/unexpected event type reaches this code, reject it.
            default: begin
              event_error <= BOOK_ERROR_INVALID_EVENT;
              state       <= ST_FINISH;
            end
          endcase
        end

        ST_SHIFT_RIGHT: begin  // This state creates a gap at insert_index.
          if (shift_index > insert_index) begin   // // Starting from the worst visible level, copy each level one slot toward the end.
            if (side_is_bid()) begin  // For a bid event, move one bid level down one slot. The ask else does the same for asks.
              work_bids[shift_index] <= work_bids[shift_index - 1];
            end else begin
              work_asks[shift_index] <= work_asks[shift_index - 1];
            end
            shift_index <= shift_index - 1'b1;  // Decrement the shift index to move toward the insertion point.
          end else begin
            // Once the shift index reaches the insertion point, write the new level into the gap.
            if (side_is_bid()) begin
              work_bids[insert_index] <= '{
                price_ticks: current_event.price_ticks,
                quantity: current_event.quantity
              };
            end else begin
              work_asks[insert_index] <= '{
                price_ticks: current_event.price_ticks,
                quantity: current_event.quantity
              };
            end

            // Record the new visible quantity as order flow, mark the candidate book changed, and validate it.
            order_flow_delta       <= order_flow_sign() * $signed({1'b0, current_event.quantity});
            operation_changes_book <= 1'b1;
            state                  <= ST_CHECK_CROSS;
          end
        end

        ST_REMOVE: begin  // This state removes one level and keeps all active levels contiguous from index zero.
          if (shift_index < BOOK_DEPTH - 1) begin  // If the cursor is not yet at the final array slot, copy the following level over the current level.
            if (side_is_bid()) begin
              work_bids[shift_index] <= work_bids[shift_index + 1];
            end else begin
              work_asks[shift_index] <= work_asks[shift_index + 1];
            end
            shift_index <= shift_index + 1'b1;  // Increment the cursor to move toward the final slot of the array.
          end else begin  // At the final slot of the array, clear it to zero. This is necessary because its old data was shifted into an earlier slot.
            if (side_is_bid()) begin
              work_bids[BOOK_DEPTH - 1] <= '0;
            end else begin
              work_asks[BOOK_DEPTH - 1] <= '0;
            end

            // The candidate book is now compacted, and the next stage can validate it before a later commit state copies it to the live book.
            operation_changes_book <= 1'b1;
            state                  <= ST_CHECK_CROSS;
          end
        end

        ST_CHECK_CROSS: begin
          if (!ALLOW_CROSSED_BOOKS && //  If the parameter ALLOW_CROSSED_BOOKS is 0, then we check for a crossed book.
              work_bids[0].quantity != 0 &&  // If the best bid is non-zero
              work_asks[0].quantity != 0 &&  // And if the best ask is non-zero
              work_bids[0].price_ticks >= work_asks[0].price_ticks) begin  // Then we check whether the best bid is greater than or equal to the best ask. If it isn't then the book is crossed and we reject the event.
            event_error      <= BOOK_ERROR_CROSSED_BOOK;  // We set the error code to indicate that.
            //  We set the flow deltas to zero because the event was rejected and did not change the book.
            order_flow_delta <= '0;  
            trade_flow_delta <= '0;
            state            <= ST_FINISH;  // We move to the finish state to report the error and return to idle.
          end else begin  //  If the book is not crossed, then we can commit the changes to the live book. 
            state <= ST_COMMIT;
          end
        end

        ST_COMMIT: begin
          // If the event changed the book, copy the working storage to the live book. This is done in one cycle because we are not worried about timing here; the FSM is already multi-cycle.
          if (operation_changes_book) begin  
            for (int unsigned level = 0; level < BOOK_DEPTH; level++) begin
              bids[level] <= work_bids[level];
              asks[level] <= work_asks[level];
            end
          end
          state <= ST_FINISH;
        end

        ST_FINISH: begin  // Signals completion and returns to idle. The FSM will wait for the next in_valid event.
          event_done <= 1'b1;
          state      <= ST_IDLE;
        end

        // If state somehow has a value that is not one of the defined states, the module reports an invariant violation and finishes the transaction.
        default: begin
          event_error <= BOOK_ERROR_INVARIANT_VIOLATION;
          state       <= ST_FINISH;
        end
      endcase
    end
  end

endmodule
