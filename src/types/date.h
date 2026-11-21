#ifndef QUARRY_TYPES_DATE_H_
#define QUARRY_TYPES_DATE_H_

#include <arrow/result.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace quarry {

enum class IntervalUnit : uint8_t { kDay, kMonth, kYear };

// Parses "YYYY-MM-DD" (years 0001 to 9999, a real calendar day) into days since 1970-01-01, the
// value of an Arrow date32. Anything else is Invalid.
arrow::Result<int32_t> ParseDate(std::string_view yyyy_mm_dd);

// "YYYY-MM-DD".
std::string FormatDate(int32_t days);

// days + n units. A month or year step keeps the day of the month and clamps it to the last day of
// the target month, like DuckDB: 1994-01-31 + 1 month = 1994-02-28. Invalid when the result leaves
// the years 0001 to 9999.
arrow::Result<int32_t> AddInterval(int32_t days, int64_t n, IntervalUnit unit);

}  // namespace quarry

#endif  // QUARRY_TYPES_DATE_H_
