// Combinational implementation of the eight Q16.16 reference-model features.
module feature_engine (
  // These are arrays containing the bid and ask sides of the order book.
  // Each price_level_t contains: 
  // - logic signed [31:0] price_tick
  // - logic [31:0] quantity
  input  market_types_pkg::price_level_t bids [0:market_types_pkg::BOOK_DEPTH-1],
  input  market_types_pkg::price_level_t asks [0:market_types_pkg::BOOK_DEPTH-1],
  // These are accumulated statistics from the feature window.
  // They are 64-bit signed values because they may become negative and may exceed 32 bits during accumulation.
  input  logic signed [63:0] order_flow_sum,
  input  logic signed [63:0] trade_flow_sum,
  input  logic signed [63:0] midpoint_change_sum,
  // The number of samples in the current feature window.
  input  logic        [6:0]  window_size,
  // This is the output array of eight Q16.16 features, and a signal saying whether they are valid.
  output logic signed [31:0] feature_values [0:market_types_pkg::FEATURE_COUNT-1],
  output logic               feature_valid
);
  import market_types_pkg::*;
  import fixed_point_pkg::*;

  // The best bid and ask prices, widened from 32 bits to 64 bits.
  logic signed [63:0] bid_price;
  logic signed [63:0] ask_price;
  // The best bid and ask quantities, widened from 32 bits to 64 bits. Although quantities are originally unsigned, they are converted to signed 64-bit values for arithmetic.
  logic signed [63:0] bid_quantity;
  logic signed [63:0] ask_quantity;
  // The midpoint price: (best bid + best ask) / 2
  logic signed [63:0] midpoint;
  // The quantity-weighted midpoint price.
  logic signed [63:0] microprice;
  // l1_total is the quantity at the best bid and best ask: bid_quantity + ask_quantity
  logic signed [63:0] l1_total;
  // l10_total is the total quantity across all ten bid and ask levels.
  logic signed [63:0] l10_total;
  // Total bid-side and ask-side quantities across the whole book.
  logic signed [63:0] bid_total;
  logic signed [63:0] ask_total;
  // Intermediate value used to calculate the microprice.
  logic signed [63:0] weighted_price;
  // Denominator used by the midpoint-change feature.
  logic signed [63:0] volatility_denominator;
  // A safe version of window_size, where zero is replaced with one.
  logic signed [63:0] window_count;

  always_comb begin  // This describes combinational logic. There is no clock, so the hardware continuously calculates the outputs from the inputs.
    // First, every output feature is cleared to zero.
    // This is important because every output must receive a value in every combinational evaluation. Otherwise, synthesis could infer unintended latches.
    for (int unsigned index = 0; index < FEATURE_COUNT; index++) feature_values[index] = '0;  
    
    // The default assumption is that the feature vector is invalid. It becomes valid only after the checks later in the block pass.
    feature_valid = 1'b0;

    // Reads the first bid / ask price and quantities.
    // - i32_to_i64 sign-extends the 32-bit price to 64 bits. This prevents the sign from being lost during later arithmetic.
    // - u32_to_i64 converts an unsigned 32-bit quantity into a positive 64-bit signed value.
    bid_price = i32_to_i64(bids[0].price_ticks);
    ask_price = i32_to_i64(asks[0].price_ticks);
    bid_quantity = u32_to_i64(bids[0].quantity);
    ask_quantity = u32_to_i64(asks[0].quantity);

    // Calculates the total bid and ask quantities across all ten levels of the order book.
    bid_total = '0;
    ask_total = '0;
    for (int unsigned index = 0; index < BOOK_DEPTH; index++) begin
      bid_total += u32_to_i64(bids[index].quantity);
      ask_total += u32_to_i64(asks[index].quantity);
    end

    // Calculates total liquidity at level 1: 
    // - L1 total = best bid quantity + best ask quantity
    l1_total = bid_quantity + ask_quantity;

    // Calculates total visible liquidity across all ten levels on both sides:
    // - L10 total = total bid quantity + total ask quantity
    l10_total = bid_total + ask_total;

    // Calculates the midpoint price:       (The 64'sd2 is a signed 64-bit decimal constant)
    // - midpoint = (best bid + best ask) / 2
    midpoint = (bid_price + ask_price) / 64'sd2;

    // Calculates the numerator of the microprice.
    // The opposite side’s quantity is used as the weighting: 
    // microprice = (ask price × bid quantity + bid price × ask quantity) / (bid quantity + ask quantity)
    weighted_price = ask_price * bid_quantity + bid_price * ask_quantity;
    microprice = rounded_divide_i64(weighted_price, l1_total);      // rounded_divide_i64 performs rounded integer division instead of simply truncating the result.

    // Avoiding divide-by-zero errors: if window_size is zero, treat it as one for the volatility feature’s denominator.
    window_count = window_size == 0 ? 64'sd1 : $signed({57'd0, window_size});

    // Calculates the denominator for the volatility feature: midpoint * window_count
    volatility_denominator = midpoint * window_count;


    // Features are only calculated if:
    // - the best bid has nonzero quantity;
    // - the best ask has nonzero quantity;
    // - the midpoint is positive;
    // - there is positive level-1 liquidity;
    // - there is positive total book liquidity.

    // If any check fails, the defaults remain active:
    // - feature_values[*] = 0
    // - feature_valid = 0;
    if (bid_quantity != 0 && ask_quantity != 0 && midpoint > 0 && l1_total > 0 && l10_total > 0) begin
      // Every call to ratio_q16(numerator, denominator) calculates: round((numerator × 65536) / denominator) and saturates the result to a signed 32-bit value.
      feature_values[0] = ratio_q16(ask_price - bid_price, midpoint);                   // This is the normalized spread:   (ask price - bid price) / midpoint
      feature_values[1] = ratio_q16(bid_quantity - ask_quantity, l1_total);             // This is the normalized imbalance: (bid quantity - ask quantity) / level-1 total
      feature_values[2] = ratio_q16(bid_total - ask_total, l10_total);                  // This is the normalized book imbalance: (total bid quantity - total ask quantity) / level-10 total
      feature_values[3] = ratio_q16(microprice - midpoint, midpoint);                   // This is the normalized microprice: (microprice - midpoint) / midpoint
      feature_values[4] = ratio_q16(order_flow_sum, l10_total);                         // This is the normalized order flow pressure: (order flow sum) / level-10 total
      feature_values[5] = ratio_q16(trade_flow_sum, l10_total);                         // This is the normalized trade flow: (trade flow sum) / level-10 total
      feature_values[6] = ratio_q16(midpoint_change_sum, volatility_denominator);       // This is the normalized midpoint change: (midpoint change sum) / volatility denominator
      feature_values[7] = Q16_ONE;                                                      // This is a constant feature value, commonly used by a linear model to represent an intercept. Q16_ONE equals 65536, which represents 1.0.
      
      // If all validation checks passed, the feature vector is marked valid.                                           
      feature_valid = 1'b1;
    end
  end
endmodule
