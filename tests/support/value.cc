#include "support/value.h"

#include <arrow/array/builder_binary.h>
#include <arrow/array/builder_decimal.h>
#include <arrow/array/builder_primitive.h>
#include <arrow/status.h>
#include <arrow/util/decimal.h>

#include <cstring>

#include "types/date.h"

namespace quarry::test {
namespace {

template <typename T>
bool SameAlternative(const Value& a, const Value& b) {
  return std::holds_alternative<T>(a) && std::holds_alternative<T>(b);
}

arrow::Decimal128 ToArrowDecimal(Int128 raw) {
  return arrow::Decimal128(static_cast<int64_t>(raw >> 64), static_cast<uint64_t>(raw));
}

}  // namespace

bool ValuesEqual(const Value& a, const Value& b) {
  if (a.index() != b.index()) {
    return false;
  }
  if (SameAlternative<double>(a, b)) {
    const double x = std::get<double>(a);
    const double y = std::get<double>(b);
    return std::memcmp(&x, &y, sizeof(double)) == 0;
  }
  return a == b;
}

std::string ValueToString(const Value& v) {
  struct Visitor {
    std::string operator()(std::monostate) const { return "NULL"; }
    std::string operator()(bool x) const { return x ? "true" : "false"; }
    std::string operator()(int32_t x) const { return std::to_string(x); }
    std::string operator()(int64_t x) const { return std::to_string(x) + "L"; }
    std::string operator()(const DecimalValue& x) const { return DecimalToString(x.raw, x.scale); }
    std::string operator()(double x) const { return std::to_string(x) + "d"; }
    std::string operator()(DateValue x) const { return FormatDate(x.days); }
    std::string operator()(const std::string& x) const { return "'" + x + "'"; }
  };
  return std::visit(Visitor{}, v);
}

arrow::Result<std::shared_ptr<arrow::Array>> MakeArray(const LogicalType& type,
                                                       const std::vector<Value>& values,
                                                       int64_t leading_garbage) {
  const auto type_error = [&](const Value& v) {
    return arrow::Status::Invalid("value ", ValueToString(v), " does not fit ", type.ToString());
  };
  std::vector<Value> all(static_cast<size_t>(leading_garbage), Value{});
  all.insert(all.end(), values.begin(), values.end());
  // Garbage rows are NULL: an array of any type can hold them.
  std::shared_ptr<arrow::Array> out;
  switch (type.id) {
    case TypeId::kBoolean: {
      arrow::BooleanBuilder builder;
      for (const auto& v : all) {
        if (IsNull(v))
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        else if (std::holds_alternative<bool>(v))
          ARROW_RETURN_NOT_OK(builder.Append(std::get<bool>(v)));
        else
          return type_error(v);
      }
      ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      break;
    }
    case TypeId::kInt32: {
      arrow::Int32Builder builder;
      for (const auto& v : all) {
        if (IsNull(v))
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        else if (std::holds_alternative<int32_t>(v))
          ARROW_RETURN_NOT_OK(builder.Append(std::get<int32_t>(v)));
        else
          return type_error(v);
      }
      ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      break;
    }
    case TypeId::kInt64: {
      arrow::Int64Builder builder;
      for (const auto& v : all) {
        if (IsNull(v))
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        else if (std::holds_alternative<int64_t>(v))
          ARROW_RETURN_NOT_OK(builder.Append(std::get<int64_t>(v)));
        else
          return type_error(v);
      }
      ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      break;
    }
    case TypeId::kDouble: {
      arrow::DoubleBuilder builder;
      for (const auto& v : all) {
        if (IsNull(v))
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        else if (std::holds_alternative<double>(v))
          ARROW_RETURN_NOT_OK(builder.Append(std::get<double>(v)));
        else
          return type_error(v);
      }
      ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      break;
    }
    case TypeId::kDate: {
      arrow::Date32Builder builder;
      for (const auto& v : all) {
        if (IsNull(v))
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        else if (std::holds_alternative<DateValue>(v))
          ARROW_RETURN_NOT_OK(builder.Append(std::get<DateValue>(v).days));
        else
          return type_error(v);
      }
      ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      break;
    }
    case TypeId::kVarchar: {
      arrow::StringBuilder builder;
      for (const auto& v : all) {
        if (IsNull(v))
          ARROW_RETURN_NOT_OK(builder.AppendNull());
        else if (std::holds_alternative<std::string>(v))
          ARROW_RETURN_NOT_OK(builder.Append(std::get<std::string>(v)));
        else
          return type_error(v);
      }
      ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      break;
    }
    case TypeId::kDecimal: {
      if (type.is_decimal64()) {
        arrow::Decimal64Builder builder(type.ToArrow());
        for (const auto& v : all) {
          if (IsNull(v)) {
            ARROW_RETURN_NOT_OK(builder.AppendNull());
          } else if (std::holds_alternative<DecimalValue>(v)) {
            ARROW_RETURN_NOT_OK(builder.Append(
                arrow::Decimal64(static_cast<int64_t>(std::get<DecimalValue>(v).raw))));
          } else {
            return type_error(v);
          }
        }
        ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      } else {
        arrow::Decimal128Builder builder(type.ToArrow());
        for (const auto& v : all) {
          if (IsNull(v)) {
            ARROW_RETURN_NOT_OK(builder.AppendNull());
          } else if (std::holds_alternative<DecimalValue>(v)) {
            ARROW_RETURN_NOT_OK(builder.Append(ToArrowDecimal(std::get<DecimalValue>(v).raw)));
          } else {
            return type_error(v);
          }
        }
        ARROW_ASSIGN_OR_RAISE(out, builder.Finish());
      }
      break;
    }
  }
  return out->Slice(leading_garbage, static_cast<int64_t>(values.size()));
}

arrow::Result<std::vector<Value>> ArrayToValues(const arrow::Array& array) {
  std::vector<Value> out;
  out.reserve(static_cast<size_t>(array.length()));
  for (int64_t i = 0; i < array.length(); ++i) {
    if (array.IsNull(i)) {
      out.emplace_back();
      continue;
    }
    switch (array.type_id()) {
      case arrow::Type::BOOL:
        out.emplace_back(static_cast<const arrow::BooleanArray&>(array).Value(i));
        break;
      case arrow::Type::INT32:
        out.emplace_back(static_cast<const arrow::Int32Array&>(array).Value(i));
        break;
      case arrow::Type::INT64:
        out.emplace_back(static_cast<const arrow::Int64Array&>(array).Value(i));
        break;
      case arrow::Type::DOUBLE:
        out.emplace_back(static_cast<const arrow::DoubleArray&>(array).Value(i));
        break;
      case arrow::Type::DATE32:
        out.emplace_back(DateValue{static_cast<const arrow::Date32Array&>(array).Value(i)});
        break;
      case arrow::Type::STRING:
        out.emplace_back(std::string(static_cast<const arrow::StringArray&>(array).GetView(i)));
        break;
      case arrow::Type::DECIMAL64: {
        const auto& typed = static_cast<const arrow::Decimal64Array&>(array);
        int64_t raw = 0;
        std::memcpy(&raw, typed.GetValue(i), sizeof(raw));
        out.emplace_back(
            DecimalValue{static_cast<Int128>(raw),
                         static_cast<const arrow::Decimal64Type&>(*array.type()).scale()});
        break;
      }
      case arrow::Type::DECIMAL128: {
        const auto& typed = static_cast<const arrow::Decimal128Array&>(array);
        Int128 raw = 0;
        std::memcpy(&raw, typed.GetValue(i), sizeof(raw));
        out.emplace_back(
            DecimalValue{raw, static_cast<const arrow::Decimal128Type&>(*array.type()).scale()});
        break;
      }
      default:
        return arrow::Status::Invalid("ArrayToValues: unsupported type ", array.type()->ToString());
    }
  }
  return out;
}

}  // namespace quarry::test
