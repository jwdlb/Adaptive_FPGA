#pragma once  // Prevent duplicate inclusion.

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace market_engine::market::fixed_point {

// Signed Q16.16 values: real_value = stored_value / 65536.
using Value = std::int32_t;
inline constexpr std::int64_t kScale{65536};
inline constexpr Value kOne{65536};  // The stored representation of 1.0.

// Exception thrown for invalid fixed-point operations.
class FixedPointError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Clamp a widened result to the signed 32-bit fixed-point range.
[[nodiscard]] inline Value saturate(std::int64_t value) noexcept {
    if (value > std::numeric_limits<Value>::max()) return std::numeric_limits<Value>::max();
    if (value < std::numeric_limits<Value>::min()) return std::numeric_limits<Value>::min();
    return static_cast<Value>(value);
}

// Convert an integer into Q16.16 format.
[[nodiscard]] inline Value from_integer(std::int32_t value) noexcept {
    return saturate(static_cast<std::int64_t>(value) * kScale);
}

// Convert a finite floating-point value, rounding to the nearest fixed-point value.
[[nodiscard]] inline Value from_double(double value) {
    if (!std::isfinite(value)) throw FixedPointError("cannot convert a non-finite value to Q16.16");
    const double scaled = value * static_cast<double>(kScale);
    if (scaled >= static_cast<double>(std::numeric_limits<Value>::max())) return std::numeric_limits<Value>::max();
    if (scaled <= static_cast<double>(std::numeric_limits<Value>::min())) return std::numeric_limits<Value>::min();
    return static_cast<Value>(std::round(scaled));
}

// Convert a Q16.16 value back to double.
[[nodiscard]] inline double to_double(Value value) noexcept {
    return static_cast<double>(value) / static_cast<double>(kScale);
}

// Divide with nearest-integer rounding; exact halves round away from zero.
[[nodiscard]] inline std::int64_t rounded_divide(std::int64_t numerator, std::int64_t denominator) noexcept {
    const bool negative = (numerator < 0) != (denominator < 0);
    const std::int64_t numerator_magnitude = numerator < 0 ? -numerator : numerator;
    const std::int64_t denominator_magnitude = denominator < 0 ? -denominator : denominator;
    std::int64_t quotient = numerator_magnitude / denominator_magnitude;
    const std::int64_t remainder = numerator_magnitude % denominator_magnitude;
    if (remainder >= (denominator_magnitude + 1) / 2) ++quotient;
    return negative ? -quotient : quotient;
}

// Multiply two Q16.16 values using a 64-bit intermediate result.
[[nodiscard]] inline Value multiply(Value left, Value right) noexcept {
    return saturate(rounded_divide(static_cast<std::int64_t>(left) * right, kScale));
}

// Divide two Q16.16 values; division by zero throws FixedPointError.
[[nodiscard]] inline Value divide(Value numerator, Value denominator) {
    if (denominator == 0) throw FixedPointError("division by zero in Q16.16 division");
    return saturate(rounded_divide(static_cast<std::int64_t>(numerator) * kScale, denominator));
}

}  // namespace market_engine::market::fixed_point
