#ifndef QUARRY_TYPES_LOGICAL_TYPE_H_
#define QUARRY_TYPES_LOGICAL_TYPE_H_

#include <arrow/result.h>
#include <arrow/type.h>

#include <cstdint>
#include <memory>
#include <string>

namespace quarry {

enum class TypeId : uint8_t { kBoolean, kInt32, kInt64, kDecimal, kDouble, kDate, kVarchar };

// The largest DECIMAL precision, and the largest one stored in a 64-bit integer.
inline constexpr int kMaxDecimalPrecision = 38;
inline constexpr int kMaxInt64DecimalPrecision = 18;

// A SQL type of the engine. precision and scale are meaningful for kDecimal only and are zero for
// every other type, so that operator== is a plain member-wise comparison.
struct LogicalType {
  TypeId id = TypeId::kBoolean;
  uint8_t precision = 0;
  uint8_t scale = 0;

  static LogicalType Boolean() { return {TypeId::kBoolean, 0, 0}; }
  static LogicalType Int32() { return {TypeId::kInt32, 0, 0}; }
  static LogicalType Int64() { return {TypeId::kInt64, 0, 0}; }
  static LogicalType Double() { return {TypeId::kDouble, 0, 0}; }
  static LogicalType Date() { return {TypeId::kDate, 0, 0}; }
  static LogicalType Varchar() { return {TypeId::kVarchar, 0, 0}; }
  static LogicalType Decimal(int precision, int scale) {
    return {TypeId::kDecimal, static_cast<uint8_t>(precision), static_cast<uint8_t>(scale)};
  }

  bool is_decimal() const { return id == TypeId::kDecimal; }
  bool is_integer() const { return id == TypeId::kInt32 || id == TypeId::kInt64; }
  // A DECIMAL whose scaled integer fits in 64 bits.
  bool is_decimal64() const { return is_decimal() && precision <= kMaxInt64DecimalPrecision; }

  // DECIMAL with p <= 18 is decimal64(p, s) and p <= 38 is decimal128(p, s); DATE is date32.
  std::shared_ptr<arrow::DataType> ToArrow() const;
  // INTEGER, BIGINT, DECIMAL(p,s), DOUBLE, DATE, VARCHAR, BOOLEAN: the names DuckDB prints.
  std::string ToString() const;
  // Accepts decimal64 and decimal128 with precision <= 18 as DECIMAL(p, s) too: the Parquet
  // reader returns decimal128 for every DECIMAL column. Everything else outside the engine's
  // types (FLOAT, UINT64, TIMESTAMP, LIST, DECIMAL with p > 38, ...) is an Invalid status that
  // names the Arrow type.
  static arrow::Result<LogicalType> FromArrow(const arrow::DataType& type);

  friend bool operator==(const LogicalType&, const LogicalType&) = default;
};

}  // namespace quarry

#endif  // QUARRY_TYPES_LOGICAL_TYPE_H_
