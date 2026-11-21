#include "types/logical_type.h"

#include <arrow/status.h>

#include <string>

namespace quarry {

std::shared_ptr<arrow::DataType> LogicalType::ToArrow() const {
  switch (id) {
    case TypeId::kBoolean:
      return arrow::boolean();
    case TypeId::kInt32:
      return arrow::int32();
    case TypeId::kInt64:
      return arrow::int64();
    case TypeId::kDecimal:
      if (precision <= kMaxInt64DecimalPrecision) {
        return arrow::decimal64(precision, scale);
      }
      return arrow::decimal128(precision, scale);
    case TypeId::kDouble:
      return arrow::float64();
    case TypeId::kDate:
      return arrow::date32();
    case TypeId::kVarchar:
      return arrow::utf8();
  }
  return arrow::null();
}

std::string LogicalType::ToString() const {
  switch (id) {
    case TypeId::kBoolean:
      return "BOOLEAN";
    case TypeId::kInt32:
      return "INTEGER";
    case TypeId::kInt64:
      return "BIGINT";
    case TypeId::kDecimal:
      return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
    case TypeId::kDouble:
      return "DOUBLE";
    case TypeId::kDate:
      return "DATE";
    case TypeId::kVarchar:
      return "VARCHAR";
  }
  return "UNKNOWN";
}

arrow::Result<LogicalType> LogicalType::FromArrow(const arrow::DataType& type) {
  switch (type.id()) {
    case arrow::Type::BOOL:
      return Boolean();
    case arrow::Type::INT32:
      return Int32();
    case arrow::Type::INT64:
      return Int64();
    case arrow::Type::DOUBLE:
      return Double();
    case arrow::Type::DATE32:
      return Date();
    case arrow::Type::STRING:
      return Varchar();
    case arrow::Type::DECIMAL64:
    case arrow::Type::DECIMAL128: {
      const auto& decimal = static_cast<const arrow::DecimalType&>(type);
      if (decimal.precision() < 1 || decimal.precision() > kMaxDecimalPrecision ||
          decimal.scale() < 0 || decimal.scale() > decimal.precision() ||
          (type.id() == arrow::Type::DECIMAL64 &&
           decimal.precision() > kMaxInt64DecimalPrecision)) {
        return arrow::Status::Invalid("unsupported type ", type.ToString());
      }
      return Decimal(decimal.precision(), decimal.scale());
    }
    default:
      return arrow::Status::Invalid("unsupported type ", type.ToString());
  }
}

}  // namespace quarry
