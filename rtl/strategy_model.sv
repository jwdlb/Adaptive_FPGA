// Eight-weight Q16.16 linear model with atomic active/shadow parameter banks.
module strategy_model (
  // Clock and active-low reset.
  input  logic               clk,
  input  logic               rst_n,
  // Requests that the model calculate and register a new result.
  input  logic               evaluate,
  // The eight Q16.16 features from feature_engine.
  input  logic signed [31:0] feature_values [0:market_types_pkg::FEATURE_COUNT-1],
  // Indicates whether the features are valid.
  input  logic               feature_valid,
  // The parameter interface:
  // The The model parameters are written one at a time:
  // - 0-7 index: Feature weights
  // - 8 index: Buy threshold
  // - 9 index: Sell threshold
  input  logic               param_write_valid,
  input  logic        [3:0]  param_write_index,
  input  logic signed [31:0] param_write_value,
  input  logic        [63:0] param_write_model_version,
  input  logic               param_commit,
  // The final result from the weighted feature calculation, the score is only updated when evaluate is asserted.
  output logic signed [31:0] score,
  // The action to take based on the score and thresholds
  output market_types_pkg::action_t action,
  // Tells the rest of the system whether the current score and action are trustworthy. 1 = yes, 0 = no
  output logic               signal_valid,
  // Identifies which version of the model is currently active. 
  // When a new set of weights and thresholds is committed: model_version <= shadow_model_version;
  // This allows external software or monitoring logic to know which model produced the current decisions.
  output logic        [63:0] model_version,
  // Counts how many complete parameter updates have successfully been committed.
  // Every successful commit performs: update_count <= update_count + 64'd1;
  output logic        [63:0] update_count
);
  import market_types_pkg::*;
  import fixed_point_pkg::*;

  // There are two copies of the weights. The active weights are currently used for scoring.
  // The shadow weights are used to load a new model safely. They do not affect scoring until committed.
  logic signed [31:0] active_weights [0:FEATURE_COUNT-1];
  logic signed [31:0] shadow_weights [0:FEATURE_COUNT-1];

  // The active and shadow BUY/SELL thresholds work the same way.
  logic signed [31:0] active_buy_threshold;
  logic signed [31:0] active_sell_threshold;
  logic signed [31:0] shadow_buy_threshold;
  logic signed [31:0] shadow_sell_threshold;
  logic        [63:0] shadow_model_version;

  // This is a ten-bit tracking register:
  // bits 0–7 = weights received
  // bit 8     = BUY threshold received
  // bit 9     = SELL threshold received
  // A new model is complete when all ten bits are set.
  logic        [9:0]  shadow_written;

  // These are 64-bit intermediate values used to avoid overflowing the 32-bit feature/score format during multiplication and accumulation.
  logic signed [63:0] score_accumulator;
  logic signed [63:0] product;

  // This performs score = Σ(weight × feature) but with fixed-point arithmetic.
  always_comb begin  // The score calculation continuously responds to the inputs.
    score_accumulator = '0;
    for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin 
      product = active_weights[index] * feature_values[index];  // Multiplies each active weight by its corresponding feature.
      score_accumulator += rounded_divide_i64(product, Q16_SCALE);   // Divides by 65536 to convert the product back to Q16.16 before adding it to the score.
    end
  end  // The result is combinational, but the output score is only updated later when evaluate is asserted.

  always_ff @(posedge clk) begin  // State changes only happen on rising clock edges.
    // Reset all state to known values on reset. 
    if (!rst_n) begin
      score <= '0;
      action <= ACTION_HOLD;
      signal_valid <= 1'b0;
      model_version <= '0;
      update_count <= '0;
      active_buy_threshold <= '0;
      active_sell_threshold <= '0;
      shadow_buy_threshold <= '0;
      shadow_sell_threshold <= '0;
      shadow_model_version <= '0;
      shadow_written <= '0;
      for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin
        active_weights[index] <= '0;
        shadow_weights[index] <= '0;
      end
    // If not in reset, handle parameter writes, commits, and evaluations.
    end else begin
      // Handle parameter writes. Each write updates the shadow bank and marks the corresponding bit in shadow_written.
      if (param_write_valid) begin
        // For indices 0 through 7:
        if (param_write_index < 4'd8) begin
          shadow_weights[param_write_index[2:0]] <= param_write_value;   // [2:0] is used to index 0-7, as that's the width of the weights array, and teh index is normally 4 bits wide. The upper bit is ignored as we know form the if statement the index is 0-7.
          shadow_written[param_write_index] <= 1'b1;
        // For index 8, update the shadow buy threshold.
        end else if (param_write_index == 4'd8) begin
          shadow_buy_threshold <= param_write_value;
          shadow_written[8] <= 1'b1;
        // For index 9, update the shadow sell threshold.
        end else if (param_write_index == 4'd9) begin
          shadow_sell_threshold <= param_write_value;
          shadow_written[9] <= 1'b1;
        end
        shadow_model_version <= param_write_model_version;  // Stores the version number for the model being loaded.
      end

      // A commit happens only when:
      // - param_commit is high;
      // - all ten parameters have been written;
      // - evaluation is not happening at the same time.
      if (param_commit && (&shadow_written) && !evaluate) begin
        // When a commit occurs, the shadow weights and thresholds are copied to the active bank, the model version is updated, and the update count is incremented.
        for (int unsigned index = 0; index < FEATURE_COUNT; index++) begin
          active_weights[index] <= shadow_weights[index];
        end
        active_buy_threshold <= shadow_buy_threshold;
        active_sell_threshold <= shadow_sell_threshold;
        model_version <= shadow_model_version;
        update_count <= update_count + 64'd1;
        // After a successful commit, the shadow_written register is cleared to prepare for the next model load.
        shadow_written <= '0;
      end

      // The model registers a result only when evaluation is requested.
      if (evaluate) begin
        // If the features are invalid no valid trading signal is produced.
        if (!feature_valid) begin
          score <= '0;
          action <= ACTION_HOLD;
          signal_valid <= 1'b0;
        // If the features are valid
        end else begin
          // The wide accumulated score is safely reduced to a signed 32-bit value.
          score <= saturate_i64_to_i32(score_accumulator);
          signal_valid <= 1'b1;
          // Then the score is compared against the active thresholds:
          if (saturate_i64_to_i32(score_accumulator) > active_buy_threshold) action <= ACTION_BUY;
          else if (saturate_i64_to_i32(score_accumulator) < active_sell_threshold) action <= ACTION_SELL;
          else action <= ACTION_HOLD;
        end
      end
    end
  end
endmodule
