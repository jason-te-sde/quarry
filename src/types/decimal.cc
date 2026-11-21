#include "types/decimal.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>

namespace quarry {

std::string DecimalToString(Int128 value, int scale) {
  const bool negative = value < 0;
  UInt128 magnitude = negative ? -static_cast<UInt128>(value) : static_cast<UInt128>(value);
  std::string digits;
  do {
    digits.push_back(static_cast<char>('0' + static_cast<int>(magnitude % 10)));
    magnitude /= 10;
  } while (magnitude != 0);
  while (static_cast<int>(digits.size()) <= scale) {
    digits.push_back('0');
  }
  std::reverse(digits.begin(), digits.end());
  if (scale > 0) {
    digits.insert(digits.size() - static_cast<size_t>(scale), ".");
  }
  return negative ? "-" + digits : digits;
}

double DecimalToDouble(Int128 value, int scale) {
  // With |value| <= 2^53 and 10^scale exactly a double (scale <= 22), one division of two exact
  // operands is correctly rounded.
  constexpr Int128 kExactLimit = 9007199254740992;  // 2^53
  constexpr int kExactScale = 22;
  if (scale <= kExactScale && value <= kExactLimit && value >= -kExactLimit) {
    constexpr std::array<double, kExactScale + 1> kPowers = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return static_cast<double>(value) / kPowers[static_cast<size_t>(scale)];
  }
  // strtod rounds the decimal text once; the program never changes the C locale.
  return std::strtod(DecimalToString(value, scale).c_str(), nullptr);
}

arrow::Result<Int128> ParseDecimalDigits(std::string_view text, int* scale) {
  size_t pos = 0;
  bool negative = false;
  if (pos < text.size() && (text[pos] == '-' || text[pos] == '+')) {
    negative = text[pos] == '-';
    ++pos;
  }
  Int128 value = 0;
  int digit_count = 0;
  int fraction_digits = 0;
  bool seen_point = false;
  bool seen_digit = false;
  for (; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '.' && !seen_point) {
      seen_point = true;
      continue;
    }
    if (c < '0' || c > '9') {
      return arrow::Status::Invalid("invalid decimal literal '", text, "'");
    }
    seen_digit = true;
    ++digit_count;
    if (digit_count > kMaxDecimalPrecision) {
      return arrow::Status::Invalid("decimal literal '", text, "' has more than ",
                                    kMaxDecimalPrecision, " digits");
    }
    value = value * 10 + (c - '0');
    if (seen_point) {
      ++fraction_digits;
    }
  }
  if (!seen_digit) {
    return arrow::Status::Invalid("invalid decimal literal '", text, "'");
  }
  *scale = fraction_digits;
  return negative ? -value : value;
}

}  // namespace quarry
