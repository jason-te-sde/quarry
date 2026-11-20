#include "types/date.h"

#include <arrow/status.h>

#include <array>
#include <cstdio>

namespace quarry {
namespace {

constexpr int kMinYear = 1;
constexpr int kMaxYear = 9999;

bool IsLeapYear(int64_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

int DaysInMonth(int64_t year, int month) {
  constexpr std::array<int, 12> kDays = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && IsLeapYear(year)) {
    return 29;
  }
  return kDays[static_cast<size_t>(month - 1)];
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's civil-days algorithm).
int64_t DaysFromCivil(int64_t year, int month, int day) {
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t year_of_era = year - era * 400;
  const int64_t shifted_month = month + (month > 2 ? -3 : 9);
  const int64_t day_of_year = (153 * shifted_month + 2) / 5 + day - 1;
  const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + day_of_era - 719468;
}

struct Civil {
  int64_t year;
  int month;
  int day;
};

Civil CivilFromDays(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t day_of_era = days - era * 146097;
  const int64_t year_of_era =
      (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
  const int64_t day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const int64_t shifted_month = (5 * day_of_year + 2) / 153;
  const int day = static_cast<int>(day_of_year - (153 * shifted_month + 2) / 5 + 1);
  const int month = static_cast<int>(shifted_month < 10 ? shifted_month + 3 : shifted_month - 9);
  const int64_t year = year_of_era + era * 400 + (month <= 2 ? 1 : 0);
  return {year, month, day};
}

bool ParseDigits(std::string_view text, int* out) {
  int value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

}  // namespace

arrow::Result<int32_t> ParseDate(std::string_view text) {
  int year = 0;
  int month = 0;
  int day = 0;
  if (text.size() != 10 || text[4] != '-' || text[7] != '-' ||
      !ParseDigits(text.substr(0, 4), &year) || !ParseDigits(text.substr(5, 2), &month) ||
      !ParseDigits(text.substr(8, 2), &day)) {
    return arrow::Status::Invalid("invalid date '", text, "': expected YYYY-MM-DD");
  }
  if (year < kMinYear || month < 1 || month > 12 || day < 1 || day > DaysInMonth(year, month)) {
    return arrow::Status::Invalid("invalid date '", text, "': no such calendar day");
  }
  return static_cast<int32_t>(DaysFromCivil(year, month, day));
}

std::string FormatDate(int32_t days) {
  const Civil civil = CivilFromDays(days);
  std::array<char, 32> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%04lld-%02d-%02d",
                static_cast<long long>(civil.year), civil.month, civil.day);
  return buffer.data();
}

arrow::Result<int32_t> AddInterval(int32_t days, int64_t n, IntervalUnit unit) {
  // Every valid date lies within 2^22 days of the epoch, so a larger step is out of range, and
  // the arithmetic below cannot overflow.
  constexpr int64_t kMaxSteps = int64_t{12} * kMaxYear;
  const int64_t limit = unit == IntervalUnit::kDay     ? int64_t{1} << 40
                        : unit == IntervalUnit::kMonth ? kMaxSteps
                                                       : int64_t{kMaxYear};
  if (n > limit || n < -limit) {
    return arrow::Status::Invalid("date out of range after adding the interval");
  }
  int64_t result = 0;
  if (unit == IntervalUnit::kDay) {
    result = static_cast<int64_t>(days) + n;
  } else {
    const int64_t months = unit == IntervalUnit::kMonth ? n : n * 12;
    const Civil civil = CivilFromDays(days);
    const int64_t total = civil.year * 12 + (civil.month - 1) + months;
    const int64_t year = total >= 0 ? total / 12 : -((-total + 11) / 12);
    const int month = static_cast<int>(total - year * 12) + 1;
    if (year < kMinYear || year > kMaxYear) {
      return arrow::Status::Invalid("date out of range after adding the interval");
    }
    const int day = civil.day < DaysInMonth(year, month) ? civil.day : DaysInMonth(year, month);
    result = DaysFromCivil(year, month, day);
  }
  const int64_t first = DaysFromCivil(kMinYear, 1, 1);
  const int64_t last = DaysFromCivil(kMaxYear, 12, 31);
  if (result < first || result > last) {
    return arrow::Status::Invalid("date out of range after adding the interval");
  }
  return static_cast<int32_t>(result);
}

}  // namespace quarry
