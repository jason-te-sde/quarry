#include "types/decimal.h"

#include <algorithm>
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
