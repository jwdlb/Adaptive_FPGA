// Fixed-point arithmetic helpers shared by the RTL signal path.
//
// The feature values use Q16.16 format:
//   * 16 bits are used for the whole-number portion;
//   * 16 bits are used for the fractional portion;
//   * therefore, the stored integer is the real value multiplied by 65536.
//
// For example:
//   * 32'sd65536 represents 1.0;
//   * 32'sd32768 represents 0.5;
//   * -32'sd65536 represents -1.0.
//
// These functions also match the software reference model. They use widened
// 64-bit intermediates, round to the nearest result, and saturate results that
// do not fit into a signed 32-bit output.
package fixed_point_pkg;
  // Scale used to convert a normal integer ratio into Q16.16 format.
  localparam logic signed [63:0] Q16_SCALE = 64'sd65536;

  // The Q16.16 representation of the real number 1.0.
  localparam logic signed [31:0] Q16_ONE   = 32'sd65536;

  // Convert an unsigned 32-bit value into a signed 64-bit value.
  //
  // The 32 zeroes on the left ensure that the original unsigned value is
  // treated as positive rather than being sign-extended from bit 31.
  function automatic logic signed [63:0] u32_to_i64(input logic [31:0] value);
    return $signed({32'd0, value});
  endfunction

  // Convert a signed 32-bit value into a signed 64-bit value.
  //
  // Replicating value[31] into the upper 32 bits is sign extension. This
  // preserves negative values when the value is used in wider arithmetic.
  function automatic logic signed [63:0] i32_to_i64(input logic signed [31:0] value);
    return {{32{value[31]}}, value};
  endfunction

  // Reduce a signed 64-bit result to signed 32 bits without wrapping.
  //
  // Normal two's-complement overflow would wrap a large positive number to a
  // negative number, or vice versa. Saturation avoids that: values above the
  // maximum stay at 0x7fffffff, and values below the minimum stay at 0x80000000.
  function automatic logic signed [31:0] saturate_i64_to_i32(input logic signed [63:0] value);
    if (value > 64'sd2147483647) return 32'sh7fff_ffff;
    if (value < -64'sd2147483648) return 32'sh8000_0000;

    // The value is known to fit, so the low 32 bits contain the result.
    return value[31:0];
  endfunction

  // Divide two signed 64-bit integers and round to the nearest integer.
  //
  // SystemVerilog integer division truncates, so this function calculates the
  // remainder itself and uses it to decide whether the quotient should be
  // increased. The sign is handled separately so positive and negative values
  // are rounded consistently.
  function automatic logic signed [63:0] rounded_divide_i64(
      input logic signed [63:0] numerator,
      input logic signed [63:0] denominator
  );
    logic negative;
    logic signed [63:0] numerator_magnitude;
    logic signed [63:0] denominator_magnitude;
    logic signed [63:0] quotient;
    logic signed [63:0] remainder;

    // Division by zero is made safe by returning zero.
    if (denominator == 0) return '0;

    // The result is negative when exactly one input is negative.
    negative = (numerator < 0) != (denominator < 0);

    // Do the division using positive magnitudes. This makes the remainder
    // comparison below easier and avoids signed-rounding surprises.
    numerator_magnitude = numerator < 0 ? -numerator : numerator;
    denominator_magnitude = denominator < 0 ? -denominator : denominator;

    quotient = numerator_magnitude / denominator_magnitude;
    remainder = numerator_magnitude % denominator_magnitude;

    // Round up when the remainder is at least half of the denominator.
    // The expression (denominator + 1) / 2 implements nearest rounding with
    // exact halves rounded away from zero after the sign is restored.
    if (remainder >= ((denominator_magnitude + 64'sd1) / 64'sd2)) quotient++;

    // Restore the original sign after rounding the magnitude.
    return negative ? -quotient : quotient;
  endfunction

  // Calculate a ratio and return it in Q16.16 format.
  //
  // Mathematically this performs:
  //
  //       numerator
  //   ----------------- × 65536
  //       denominator
  //
  // The multiplication happens before division so the fractional part is
  // preserved. The final result is rounded and saturated to signed 32 bits.
  function automatic logic signed [31:0] ratio_q16(
      input logic signed [63:0] numerator,
      input logic signed [63:0] denominator
  );
    logic signed [63:0] scaled;
    // A non-positive denominator is invalid for the ratios used by the
    // feature engine, so return a safe zero result.
    if (denominator <= 0) return '0;

    // Scale first to convert the ordinary ratio into Q16.16 representation.
    scaled = rounded_divide_i64(numerator * Q16_SCALE, denominator);

    // Convert the widened result to the feature output width safely.
    return saturate_i64_to_i32(scaled);
  endfunction

  // Multiply two Q16.16 values and return another Q16.16 value.
  //
  // Multiplying two fixed-point values produces an extra factor of 65536:
  //
  //   (left × 65536) × (right × 65536)
  //   ---------------------------------- = real_product × 65536²
  //
  // Dividing by Q16_SCALE removes one of those scale factors. The result is
  // then rounded and saturated to the signed 32-bit feature format.
  function automatic logic signed [31:0] multiply_q16(
      input logic signed [31:0] left,
      input logic signed [31:0] right
  );
    logic signed [63:0] product;
    // Widen the product so the intermediate multiplication has room to grow.
    product = left * right;

    // Remove the extra Q16 scaling factor and safely return the result.
    return saturate_i64_to_i32(rounded_divide_i64(product, Q16_SCALE));
  endfunction
endpackage
