#ifndef QUARRY_TYPES_DECIMAL_H_
#define QUARRY_TYPES_DECIMAL_H_

#include <arrow/result.h>
#include <arrow/status.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "common/flaws.h"
#include "types/logical_type.h"

namespace quarry {

// A DECIMAL(p, s) value is the integer value * 10^s: an int64_t when p <= 18 and a 128-bit integer
// when p <= 38. __int128 is a compiler extension; the typedef keeps -Wpedantic quiet.
__extension__ using Int128 = __int128;
__extension__ using UInt128 = unsigned __int128;

inline constexpr Int128 kInt128Max = static_cast<Int128>(~static_cast<UInt128>(0) >> 1);

namespace decimal_internal {

constexpr std::array<Int128, kMaxDecimalPrecision + 1> MakePowersOfTen() {
  std::array<Int128, kMaxDecimalPrecision + 1> powers{};
  powers[0] = 1;
  for (size_t i = 1; i < powers.size(); ++i) {
    powers[i] = powers[i - 1] * 10;
  }
  return powers;
}

inline constexpr std::array<Int128, kMaxDecimalPrecision + 1> kPowersOfTen = MakePowersOfTen();

}  // namespace decimal_internal

// 10^n for 0 <= n <= 38 (n <= 18 for int64_t).
template <typename T>
constexpr T PowerOfTen(int n) {
  return static_cast<T>(decimal_internal::kPowersOfTen[static_cast<size_t>(n)]);
}

// True when |v| < 10^precision, i.e. v has at most `precision` digits.
template <typename T>
constexpr bool FitsPrecision(T v, int precision) {
  const T limit = PowerOfTen<T>(precision);
  return v < limit && v > -limit;
}

inline arrow::Status DecimalOverflow(const char* what, int precision) {
  return arrow::Status::Invalid("decimal overflow in ", what, " of DECIMAL(", precision, ")");
}

// Checked arithmetic on scaled integers of one scale: the result must fit `precision` digits,
// otherwise the status is Invalid and says "decimal overflow". Nothing wraps around.
template <typename T>
arrow::Result<T> DecimalAdd(T a, T b, int precision) {
  T out;
  if (__builtin_add_overflow(a, b, &out) || !FitsPrecision(out, precision)) {
    return DecimalOverflow("addition", precision);
  }
  return out;
}

template <typename T>
arrow::Result<T> DecimalSub(T a, T b, int precision) {
  T out;
  if (__builtin_sub_overflow(a, b, &out) || !FitsPrecision(out, precision)) {
    return DecimalOverflow("subtraction", precision);
  }
  return out;
}

// The product of two scaled integers has the scale s1 + s2; the caller passes the result's
// precision.
template <typename T>
arrow::Result<T> DecimalMul(T a, T b, int precision) {
  T out;
  if (__builtin_mul_overflow(a, b, &out) || !FitsPrecision(out, precision)) {
    return DecimalOverflow("multiplication", precision);
  }
  return out;
}

// Changes the scale of a scaled integer from `from_scale` to `to_scale`. Dropping digits rounds
// half away from zero (DuckDB's rule: 1.005 -> 1.01, -2.5 -> -3). Returns false when the value
// does not fit T. Does not check a precision: the caller does.
template <typename T>
inline bool RescaleValue(T v, int from_scale, int to_scale, T* out) {
  int diff = to_scale - from_scale;
  if constexpr (kFlawDecimalScaleSlip) {
    if (diff >= 2) {
      --diff;
    } else if (diff <= -2) {
      ++diff;
    }
  }
  if (diff >= 0) {
    return !__builtin_mul_overflow(v, PowerOfTen<T>(diff), out);
  }
  const T divisor = PowerOfTen<T>(-diff);
  T quotient = v / divisor;
  const T remainder = v % divisor;
  const T magnitude = remainder < 0 ? -remainder : remainder;
  if (magnitude >= divisor - magnitude) {
    quotient += v < 0 ? -1 : 1;
  }
  *out = quotient;
  return true;
}

// RescaleValue plus the precision check, with the "decimal overflow" status.
template <typename T>
arrow::Result<T> DecimalRescale(T v, int from_scale, int to_scale, int to_precision) {
  T out;
  if (!RescaleValue(v, from_scale, to_scale, &out) || !FitsPrecision(out, to_precision)) {
    return DecimalOverflow("rescale", to_precision);
  }
  return out;
}

// "-123.45" for (-12345, 2); "7" for (7, 0).
std::string DecimalToString(Int128 value, int scale);

// value / 10^scale as the double nearest to it, rounded once, as DuckDB does. `(double)value /
// 10^scale` rounds twice when |value| > 2^53 (the numerator is rounded before the division), and
// 10^scale is not a double beyond 10^22, so those cases go through the decimal text and strtod
// instead.
double DecimalToDouble(Int128 value, int scale);

// Parses an optionally signed decimal literal such as "0.05", "-12" or "1." into the scaled
// integer at its own scale (the digits after the point). Invalid on anything else, and when the
// digits do not fit 38 digits.
arrow::Result<Int128> ParseDecimalDigits(std::string_view text, int* scale);

}  // namespace quarry

#endif  // QUARRY_TYPES_DECIMAL_H_
