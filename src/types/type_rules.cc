#include "types/type_rules.h"

#include <arrow/status.h>

#include <algorithm>

namespace quarry {
namespace {

bool IsNumber(const LogicalType& t) {
  return t.is_integer() || t.is_decimal() || t.id == TypeId::kDouble;
}

arrow::Status CannotApply(BinaryOp op, const LogicalType& l, const LogicalType& r) {
  return arrow::Status::Invalid("operator ", BinaryOpName(op), " cannot be applied to ",
                                l.ToString(), " and ", r.ToString());
}

arrow::Result<LogicalType> DecimalArithmetic(BinaryOp op, const LogicalType& l,
                                             const LogicalType& r) {
  const LogicalType a = AsDecimalType(l);
  const LogicalType b = AsDecimalType(r);
  int precision = 0;
  int scale = 0;
  if (op == BinaryOp::kMul) {
    scale = a.scale + b.scale;
    precision = a.precision + b.precision;
    if (precision > kMaxInt64DecimalPrecision && a.precision <= kMaxInt64DecimalPrecision &&
        b.precision <= kMaxInt64DecimalPrecision) {
      precision = kMaxInt64DecimalPrecision;
    }
    precision = std::max(precision, scale);
  } else {
    scale = std::max(a.scale, b.scale);
    precision = std::max(a.precision - a.scale, b.precision - b.scale) + scale + 1;
  }
  precision = std::min(precision, kMaxDecimalPrecision);
  if (scale > kMaxDecimalPrecision) {
    return arrow::Status::Invalid("result of ", l.ToString(), " ", BinaryOpName(op), " ",
                                  r.ToString(), " needs a scale of ", scale, ", above ",
                                  kMaxDecimalPrecision);
  }
  return LogicalType::Decimal(precision, scale);
}

}  // namespace

LogicalType AsDecimalType(const LogicalType& t) {
  switch (t.id) {
    case TypeId::kInt32:
      return LogicalType::Decimal(10, 0);
    case TypeId::kInt64:
      return LogicalType::Decimal(19, 0);
    default:
      return t;
  }
}

const char* BinaryOpName(BinaryOp op) {
  switch (op) {
    case BinaryOp::kAdd:
      return "+";
    case BinaryOp::kSub:
      return "-";
    case BinaryOp::kMul:
      return "*";
    case BinaryOp::kEq:
      return "=";
    case BinaryOp::kNe:
      return "<>";
    case BinaryOp::kLt:
      return "<";
    case BinaryOp::kLe:
      return "<=";
    case BinaryOp::kGt:
      return ">";
    case BinaryOp::kGe:
      return ">=";
    case BinaryOp::kAnd:
      return "AND";
    case BinaryOp::kOr:
      return "OR";
  }
  return "?";
}

bool IsComparison(BinaryOp op) {
  switch (op) {
    case BinaryOp::kEq:
    case BinaryOp::kNe:
    case BinaryOp::kLt:
    case BinaryOp::kLe:
    case BinaryOp::kGt:
    case BinaryOp::kGe:
      return true;
    default:
      return false;
  }
}

bool IsArithmetic(BinaryOp op) {
  return op == BinaryOp::kAdd || op == BinaryOp::kSub || op == BinaryOp::kMul;
}

arrow::Result<LogicalType> CommonComparisonType(const LogicalType& l, const LogicalType& r) {
  if (l == r) {
    return l;
  }
  if (IsNumber(l) && IsNumber(r)) {
    if (l.id == TypeId::kDouble || r.id == TypeId::kDouble) {
      return LogicalType::Double();
    }
    if (l.is_integer() && r.is_integer()) {
      return LogicalType::Int64();
    }
    const LogicalType a = AsDecimalType(l);
    const LogicalType b = AsDecimalType(r);
    const int scale = std::max(a.scale, b.scale);
    const int digits = std::max(a.precision - a.scale, b.precision - b.scale) + scale;
    return LogicalType::Decimal(std::min(digits, kMaxDecimalPrecision), scale);
  }
  return arrow::Status::Invalid("cannot compare ", l.ToString(), " with ", r.ToString());
}

arrow::Result<LogicalType> BinaryResultType(BinaryOp op, const LogicalType& l,
                                            const LogicalType& r) {
  if (op == BinaryOp::kAnd || op == BinaryOp::kOr) {
    if (l.id == TypeId::kBoolean && r.id == TypeId::kBoolean) {
      return LogicalType::Boolean();
    }
    return CannotApply(op, l, r);
  }
  if (IsComparison(op)) {
    auto common = CommonComparisonType(l, r);
    if (!common.ok()) {
      return common.status();
    }
    return LogicalType::Boolean();
  }
  // + - *
  if (l.id == TypeId::kDate || r.id == TypeId::kDate) {
    const bool date_with_int32 =
        (l.id == TypeId::kDate && r.id == TypeId::kInt32) ||
        (op == BinaryOp::kAdd && l.id == TypeId::kInt32 && r.id == TypeId::kDate);
    if (op != BinaryOp::kMul && date_with_int32) {
      return LogicalType::Date();
    }
    return CannotApply(op, l, r);
  }
  if (!IsNumber(l) || !IsNumber(r)) {
    return CannotApply(op, l, r);
  }
  if (l.id == TypeId::kDouble || r.id == TypeId::kDouble) {
    return LogicalType::Double();
  }
  if (l.is_integer() && r.is_integer()) {
    return (l.id == TypeId::kInt64 || r.id == TypeId::kInt64) ? LogicalType::Int64()
                                                              : LogicalType::Int32();
  }
  return DecimalArithmetic(op, l, r);
}

LogicalType DecimalLiteralType(std::string_view digits) {
  int count = 0;
  int fraction = 0;
  bool after_point = false;
  for (const char c : digits) {
    if (c == '.') {
      after_point = true;
    } else if (c >= '0' && c <= '9') {
      ++count;
      fraction += after_point ? 1 : 0;
    }
  }
  count = std::clamp(count, 1, kMaxDecimalPrecision);
  fraction = std::min(fraction, count);
  return LogicalType::Decimal(count, fraction);
}

const char* AggFuncName(AggFunc func) {
  switch (func) {
    case AggFunc::kSum:
      return "SUM";
    case AggFunc::kAvg:
      return "AVG";
    case AggFunc::kCount:
      return "COUNT";
    case AggFunc::kCountStar:
      return "COUNT(*)";
    case AggFunc::kMin:
      return "MIN";
    case AggFunc::kMax:
      return "MAX";
  }
  return "?";
}

arrow::Result<LogicalType> AggregateResultType(AggFunc func, const LogicalType& arg) {
  switch (func) {
    case AggFunc::kCount:
    case AggFunc::kCountStar:
      return LogicalType::Int64();
    case AggFunc::kMin:
    case AggFunc::kMax:
      return arg;
    case AggFunc::kSum:
    case AggFunc::kAvg:
      if (!IsNumber(arg)) {
        return arrow::Status::Invalid(AggFuncName(func), " cannot be applied to ", arg.ToString());
      }
      if (func == AggFunc::kAvg || arg.id == TypeId::kDouble) {
        return LogicalType::Double();
      }
      return LogicalType::Decimal(kMaxDecimalPrecision, arg.is_decimal() ? arg.scale : 0);
  }
  return arrow::Status::Invalid("unknown aggregate function");
}

}  // namespace quarry
