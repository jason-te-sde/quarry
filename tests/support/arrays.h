#ifndef QUARRY_TESTS_SUPPORT_ARRAYS_H_
#define QUARRY_TESTS_SUPPORT_ARRAYS_H_

// Small helpers that build Arrow arrays of the engine's types from C++ values, with NULL positions
// given as a set of indices.

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "types/decimal.h"

namespace quarry::testing {

using ArrayPtr = std::shared_ptr<arrow::Array>;
using NullSet = std::set<size_t>;

template <typename Builder, typename T>
ArrayPtr BuildWith(Builder& builder, const std::vector<T>& values, const NullSet& nulls) {
  for (size_t i = 0; i < values.size(); ++i) {
    if (nulls.count(i) > 0) {
      EXPECT_TRUE(builder.AppendNull().ok());
    } else {
      EXPECT_TRUE(builder.Append(values[i]).ok());
    }
  }
  ArrayPtr out;
  EXPECT_TRUE(builder.Finish(&out).ok());
  return out;
}

inline ArrayPtr Int32Array(const std::vector<int32_t>& values, const NullSet& nulls = {}) {
  arrow::Int32Builder builder;
  return BuildWith(builder, values, nulls);
}
inline ArrayPtr Int64Array(const std::vector<int64_t>& values, const NullSet& nulls = {}) {
  arrow::Int64Builder builder;
  return BuildWith(builder, values, nulls);
}
inline ArrayPtr DoubleArray(const std::vector<double>& values, const NullSet& nulls = {}) {
  arrow::DoubleBuilder builder;
  return BuildWith(builder, values, nulls);
}
inline ArrayPtr DateArray(const std::vector<int32_t>& days, const NullSet& nulls = {}) {
  arrow::Date32Builder builder;
  return BuildWith(builder, days, nulls);
}
inline ArrayPtr StringArray(const std::vector<std::string>& values, const NullSet& nulls = {}) {
  arrow::StringBuilder builder;
  return BuildWith(builder, values, nulls);
}
inline ArrayPtr BoolArray(const std::vector<bool>& values, const NullSet& nulls = {}) {
  arrow::BooleanBuilder builder;
  for (size_t i = 0; i < values.size(); ++i) {
    if (nulls.count(i) > 0) {
      EXPECT_TRUE(builder.AppendNull().ok());
    } else {
      EXPECT_TRUE(builder.Append(values[i]).ok());
    }
  }
  ArrayPtr out;
  EXPECT_TRUE(builder.Finish(&out).ok());
  return out;
}

// A decimal64(precision, scale) array of the scaled integers.
inline ArrayPtr Decimal64Array(const std::vector<int64_t>& scaled, int precision, int scale,
                               const NullSet& nulls = {}) {
  const auto n = static_cast<int64_t>(scaled.size());
  auto values = arrow::AllocateBuffer(n * 8).ValueOrDie();
  auto validity = arrow::AllocateEmptyBitmap(n).ValueOrDie();
  int64_t null_count = 0;
  for (size_t i = 0; i < scaled.size(); ++i) {
    std::memcpy(values->mutable_data() + i * 8, &scaled[i], 8);
    if (nulls.count(i) > 0) {
      ++null_count;
    } else {
      arrow::bit_util::SetBit(validity->mutable_data(), static_cast<int64_t>(i));
    }
  }
  return arrow::MakeArray(arrow::ArrayData::Make(
      arrow::decimal64(precision, scale), n,
      {null_count == 0 ? nullptr : std::move(validity), std::move(values)}, null_count));
}

// A decimal128(precision, scale) array of the scaled integers.
inline ArrayPtr Decimal128Array(const std::vector<Int128>& scaled, int precision, int scale,
                                const NullSet& nulls = {}) {
  const auto n = static_cast<int64_t>(scaled.size());
  auto values = arrow::AllocateBuffer(n * 16).ValueOrDie();
  auto validity = arrow::AllocateEmptyBitmap(n).ValueOrDie();
  int64_t null_count = 0;
  for (size_t i = 0; i < scaled.size(); ++i) {
    std::memcpy(values->mutable_data() + i * 16, &scaled[i], 16);
    if (nulls.count(i) > 0) {
      ++null_count;
    } else {
      arrow::bit_util::SetBit(validity->mutable_data(), static_cast<int64_t>(i));
    }
  }
  return arrow::MakeArray(arrow::ArrayData::Make(
      arrow::decimal128(precision, scale), n,
      {null_count == 0 ? nullptr : std::move(validity), std::move(values)}, null_count));
}

// Row `i` of an array as text: "NULL", or the value (decimals as digits with the point, doubles
// shortest, dates as day numbers).
inline std::string Cell(const arrow::Array& array, int64_t i) {
  if (array.IsNull(i)) return "NULL";
  switch (array.type_id()) {
    case arrow::Type::BOOL:
      return static_cast<const arrow::BooleanArray&>(array).Value(i) ? "true" : "false";
    case arrow::Type::INT32:
      return std::to_string(static_cast<const arrow::Int32Array&>(array).Value(i));
    case arrow::Type::INT64:
      return std::to_string(static_cast<const arrow::Int64Array&>(array).Value(i));
    case arrow::Type::DATE32:
      return std::to_string(static_cast<const arrow::Date32Array&>(array).Value(i));
    case arrow::Type::DOUBLE: {
      char buffer[40];
      std::snprintf(buffer, sizeof(buffer), "%.17g",
                    static_cast<const arrow::DoubleArray&>(array).Value(i));
      return buffer;
    }
    case arrow::Type::STRING:
      return std::string(static_cast<const arrow::StringArray&>(array).GetView(i));
    case arrow::Type::DECIMAL64: {
      const auto& decimal = static_cast<const arrow::Decimal64Array&>(array);
      int64_t value;
      std::memcpy(&value, decimal.GetValue(i), 8);
      return DecimalToString(value,
                             static_cast<const arrow::Decimal64Type&>(*array.type()).scale());
    }
    case arrow::Type::DECIMAL128: {
      const auto& decimal = static_cast<const arrow::Decimal128Array&>(array);
      Int128 value;
      std::memcpy(&value, decimal.GetValue(i), 16);
      return DecimalToString(value,
                             static_cast<const arrow::Decimal128Type&>(*array.type()).scale());
    }
    default:
      return "?";
  }
}

// The cells of one array, in order.
inline std::vector<std::string> Cells(const arrow::Array& array) {
  std::vector<std::string> out;
  for (int64_t i = 0; i < array.length(); ++i) out.push_back(Cell(array, i));
  return out;
}

}  // namespace quarry::testing

#endif  // QUARRY_TESTS_SUPPORT_ARRAYS_H_
