#ifndef QUARRY_TESTS_SUPPORT_VALUE_H_
#define QUARRY_TESTS_SUPPORT_VALUE_H_

#include <arrow/array.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "types/decimal.h"
#include "types/logical_type.h"

namespace quarry::test {

// A DECIMAL value: the scaled integer and its scale.
struct DecimalValue {
  Int128 raw = 0;
  int scale = 0;
  friend bool operator==(const DecimalValue&, const DecimalValue&) = default;
};

// A DATE: days since 1970-01-01.
struct DateValue {
  int32_t days = 0;
  friend bool operator==(const DateValue&, const DateValue&) = default;
};

// One SQL value as the row-at-a-time reference evaluator sees it. std::monostate is NULL.
using Value = std::variant<std::monostate, bool, int32_t, int64_t, DecimalValue, double, DateValue,
                           std::string>;

inline bool IsNull(const Value& v) { return std::holds_alternative<std::monostate>(v); }

// Equality for the tests: doubles are compared bit for bit, everything else by value. NULL equals
// NULL.
bool ValuesEqual(const Value& a, const Value& b);
std::string ValueToString(const Value& v);

// Builds an Arrow array of the engine's type for `type` with Arrow's own builders (so it shares no
// code with the engine's kernels). Every Value must be NULL or of the alternative `type` calls for.
// With `leading_garbage` > 0 the array is built with that many unrelated rows in front and sliced,
// so that it has a non-zero offset.
arrow::Result<std::shared_ptr<arrow::Array>> MakeArray(const LogicalType& type,
                                                       const std::vector<Value>& values,
                                                       int64_t leading_garbage = 0);

// The values of an array, read back through Arrow's typed array classes. Accepts the array types
// that LogicalType::ToArrow gives, and decimal128 for DECIMAL(p <= 18).
arrow::Result<std::vector<Value>> ArrayToValues(const arrow::Array& array);

}  // namespace quarry::test

#endif  // QUARRY_TESTS_SUPPORT_VALUE_H_
