#include "support/reference_eval.h"

#include <arrow/status.h>
#include <arrow/util/decimal.h>

#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>

namespace quarry::test {
namespace {

using Row = std::vector<Value>;

// 10^n built by repeated multiplication; n <= 38.
Int128 Ten(int n) {
  Int128 value = 1;
  for (int i = 0; i < n; ++i) {
    value *= 10;
  }
  return value;
}

// The double nearest to raw / 10^scale, rounded once: strtod reads the exact decimal text.
// (Dividing the double of raw by 10^scale rounds twice once raw has more than 53 bits.)
double DecimalAsDouble(Int128 raw, int scale) {
  return std::strtod(DecimalToString(raw, scale).c_str(), nullptr);
}

Int128 Abs(Int128 v) { return v < 0 ? -v : v; }

bool Fits(Int128 v, int precision) { return Abs(v) < Ten(precision); }

// Changes the scale, rounding half away from zero when digits go.
arrow::Result<Int128> ChangeScale(Int128 raw, int from, int to, int precision) {
  Int128 out = raw;
  if (to >= from) {
    for (int i = from; i < to; ++i) {
      if (Abs(out) > kInt128Max / 10) {
        return arrow::Status::Invalid("decimal overflow (reference): rescale");
      }
      out *= 10;
    }
  } else {
    const Int128 divisor = Ten(from - to);
    const Int128 quotient = raw / divisor;
    const UInt128 remainder = static_cast<UInt128>(Abs(raw % divisor));
    out = quotient;
    if (remainder * 2 >= static_cast<UInt128>(divisor)) {
      out += raw < 0 ? -1 : 1;
    }
  }
  if (!Fits(out, precision)) {
    return arrow::Status::Invalid("decimal overflow (reference): rescale to precision ", precision);
  }
  return out;
}

arrow::Result<Value> ArithmeticValues(const BoundExpr& e, const Value& a, const Value& b) {
  const BinaryOp op = e.op;
  const auto combine = [&](Int128 x, Int128 y) -> arrow::Result<Int128> {
    switch (op) {
      case BinaryOp::kAdd:
        return x + y;
      case BinaryOp::kSub:
        return x - y;
      default: {
        if (x != 0 && Abs(y) > kInt128Max / Abs(x)) {
          return arrow::Status::Invalid("overflow (reference): multiplication");
        }
        return x * y;
      }
    }
  };
  switch (e.type.id) {
    case TypeId::kInt32: {
      ARROW_ASSIGN_OR_RAISE(Int128 r, combine(std::get<int32_t>(a), std::get<int32_t>(b)));
      if (r < INT32_MIN || r > INT32_MAX)
        return arrow::Status::Invalid("integer overflow (reference)");
      return Value(static_cast<int32_t>(r));
    }
    case TypeId::kInt64: {
      ARROW_ASSIGN_OR_RAISE(Int128 r, combine(std::get<int64_t>(a), std::get<int64_t>(b)));
      if (r < INT64_MIN || r > INT64_MAX)
        return arrow::Status::Invalid("integer overflow (reference)");
      return Value(static_cast<int64_t>(r));
    }
    case TypeId::kDouble: {
      const double x = std::get<double>(a);
      const double y = std::get<double>(b);
      return Value(op == BinaryOp::kAdd ? x + y : op == BinaryOp::kSub ? x - y : x * y);
    }
    case TypeId::kDate: {
      const bool date_first = std::holds_alternative<DateValue>(a);
      const int64_t days = date_first ? std::get<DateValue>(a).days : std::get<DateValue>(b).days;
      const int64_t delta = date_first ? std::get<int32_t>(b) : std::get<int32_t>(a);
      const int64_t r = op == BinaryOp::kSub ? days - delta : days + delta;
      if (r < INT32_MIN || r > INT32_MAX)
        return arrow::Status::Invalid("integer overflow (reference)");
      return Value(DateValue{static_cast<int32_t>(r)});
    }
    case TypeId::kDecimal: {
      const auto& x = std::get<DecimalValue>(a);
      const auto& y = std::get<DecimalValue>(b);
      Int128 xr = x.raw;
      Int128 yr = y.raw;
      int scale = x.scale;
      if (op == BinaryOp::kMul) {
        scale = x.scale + y.scale;
      } else {
        scale = std::max(x.scale, y.scale);
        for (int i = x.scale; i < scale; ++i) xr *= 10;
        for (int i = y.scale; i < scale; ++i) yr *= 10;
      }
      if (scale != e.type.scale) {
        return arrow::Status::Invalid("reference: result scale ", scale, " is not ",
                                      e.type.ToString());
      }
      ARROW_ASSIGN_OR_RAISE(Int128 r, combine(xr, yr));
      if (!Fits(r, e.type.precision)) {
        return arrow::Status::Invalid("decimal overflow (reference): ", e.type.ToString());
      }
      return Value(DecimalValue{r, scale});
    }
    default:
      return arrow::Status::Invalid("reference: arithmetic on ", e.type.ToString());
  }
}

template <typename T>
int Order(const T& a, const T& b) {
  return a < b ? -1 : (b < a ? 1 : 0);
}

arrow::Result<int> Compare(const Value& a, const Value& b) {
  if (a.index() != b.index()) {
    return arrow::Status::Invalid("reference: comparing ", ValueToString(a), " with ",
                                  ValueToString(b));
  }
  if (std::holds_alternative<bool>(a)) return Order(std::get<bool>(a), std::get<bool>(b));
  if (std::holds_alternative<int32_t>(a)) return Order(std::get<int32_t>(a), std::get<int32_t>(b));
  if (std::holds_alternative<int64_t>(a)) return Order(std::get<int64_t>(a), std::get<int64_t>(b));
  if (std::holds_alternative<double>(a)) {
    const double x = std::get<double>(a);
    const double y = std::get<double>(b);
    return x < y ? -1 : (x > y ? 1 : 0);
  }
  if (std::holds_alternative<DateValue>(a))
    return Order(std::get<DateValue>(a).days, std::get<DateValue>(b).days);
  if (std::holds_alternative<std::string>(a)) {
    return std::get<std::string>(a).compare(std::get<std::string>(b)) < 0
               ? -1
               : (std::get<std::string>(a) == std::get<std::string>(b) ? 0 : 1);
  }
  const auto& x = std::get<DecimalValue>(a);
  const auto& y = std::get<DecimalValue>(b);
  if (x.scale != y.scale) {
    return arrow::Status::Invalid(
        "reference: decimals of different scales compared without a cast");
  }
  return Order(x.raw, y.raw);
}

arrow::Result<Value> CastValue(const Value& v, const LogicalType& to) {
  switch (to.id) {
    case TypeId::kInt64:
      return Value(static_cast<int64_t>(std::get<int32_t>(v)));
    case TypeId::kDouble:
      if (std::holds_alternative<int32_t>(v))
        return Value(static_cast<double>(std::get<int32_t>(v)));
      if (std::holds_alternative<int64_t>(v))
        return Value(static_cast<double>(std::get<int64_t>(v)));
      return Value(DecimalAsDouble(std::get<DecimalValue>(v).raw, std::get<DecimalValue>(v).scale));
    case TypeId::kDecimal: {
      if (std::holds_alternative<DecimalValue>(v)) {
        const auto& d = std::get<DecimalValue>(v);
        ARROW_ASSIGN_OR_RAISE(Int128 r, ChangeScale(d.raw, d.scale, to.scale, to.precision));
        return Value(DecimalValue{r, to.scale});
      }
      const Int128 whole =
          std::holds_alternative<int32_t>(v) ? std::get<int32_t>(v) : std::get<int64_t>(v);
      ARROW_ASSIGN_OR_RAISE(Int128 r, ChangeScale(whole, 0, to.scale, to.precision));
      return Value(DecimalValue{r, to.scale});
    }
    default:
      return arrow::Status::Invalid("reference: cannot cast to ", to.ToString());
  }
}

Value FromScalar(const arrow::Scalar& s) {
  if (!s.is_valid) return Value{};
  switch (s.type->id()) {
    case arrow::Type::BOOL:
      return Value(static_cast<const arrow::BooleanScalar&>(s).value);
    case arrow::Type::INT32:
      return Value(static_cast<const arrow::Int32Scalar&>(s).value);
    case arrow::Type::INT64:
      return Value(static_cast<const arrow::Int64Scalar&>(s).value);
    case arrow::Type::DOUBLE:
      return Value(static_cast<const arrow::DoubleScalar&>(s).value);
    case arrow::Type::DATE32:
      return Value(DateValue{static_cast<const arrow::Date32Scalar&>(s).value});
    case arrow::Type::STRING:
      return Value(static_cast<const arrow::StringScalar&>(s).value->ToString());
    case arrow::Type::DECIMAL64: {
      const auto& d = static_cast<const arrow::Decimal64Scalar&>(s);
      return Value(DecimalValue{static_cast<Int128>(static_cast<int64_t>(d.value)),
                                static_cast<const arrow::Decimal64Type&>(*s.type).scale()});
    }
    default: {
      const auto& d = static_cast<const arrow::Decimal128Scalar&>(s);
      const Int128 raw = (static_cast<Int128>(d.value.high_bits()) << 64) |
                         static_cast<Int128>(d.value.low_bits());
      return Value(DecimalValue{raw, static_cast<const arrow::Decimal128Type&>(*s.type).scale()});
    }
  }
}

class RowEvaluator {
 public:
  arrow::Result<Value> Eval(const BoundExpr& e, const Row& row) const {
    switch (e.kind) {
      case ExprKind::kColumn:
        return row.at(static_cast<size_t>(e.column_index));
      case ExprKind::kLiteral:
        return FromScalar(*e.literal);
      case ExprKind::kNegate: {
        ARROW_ASSIGN_OR_RAISE(Value v, Eval(*e.children[0], row));
        return Negate(v);
      }
      case ExprKind::kNot: {
        ARROW_ASSIGN_OR_RAISE(Value v, Eval(*e.children[0], row));
        if (IsNull(v)) return Value{};
        return Value(!std::get<bool>(v));
      }
      case ExprKind::kIsNull: {
        ARROW_ASSIGN_OR_RAISE(Value v, Eval(*e.children[0], row));
        return Value(IsNull(v) != e.negated);
      }
      case ExprKind::kCast: {
        ARROW_ASSIGN_OR_RAISE(Value v, Eval(*e.children[0], row));
        if (IsNull(v)) return Value{};
        if (e.children[0]->type == e.type) return v;
        return CastValue(v, e.type);
      }
      case ExprKind::kBinary:
        return Binary(e, row);
      case ExprKind::kBetween: {
        ARROW_ASSIGN_OR_RAISE(Value x, Eval(*e.children[0], row));
        ARROW_ASSIGN_OR_RAISE(Value lo, Eval(*e.children[1], row));
        ARROW_ASSIGN_OR_RAISE(Value hi, Eval(*e.children[2], row));
        ARROW_ASSIGN_OR_RAISE(Value ge, Cmp(BinaryOp::kGe, x, lo));
        ARROW_ASSIGN_OR_RAISE(Value le, Cmp(BinaryOp::kLe, x, hi));
        return Kleene(BinaryOp::kAnd, ge, le);
      }
      case ExprKind::kInList: {
        ARROW_ASSIGN_OR_RAISE(Value x, Eval(*e.children[0], row));
        Value any = Value(false);
        for (const auto& item : e.in_list) {
          ARROW_ASSIGN_OR_RAISE(Value eq, Cmp(BinaryOp::kEq, x, FromScalar(*item)));
          any = Kleene(BinaryOp::kOr, any, eq);
        }
        return any;
      }
    }
    return arrow::Status::Invalid("reference: unknown kind");
  }

 private:
  static arrow::Result<Value> Negate(const Value& v) {
    if (IsNull(v)) return Value{};
    if (std::holds_alternative<int32_t>(v)) {
      if (std::get<int32_t>(v) == INT32_MIN)
        return arrow::Status::Invalid("integer overflow (reference)");
      return Value(-std::get<int32_t>(v));
    }
    if (std::holds_alternative<int64_t>(v)) {
      if (std::get<int64_t>(v) == INT64_MIN)
        return arrow::Status::Invalid("integer overflow (reference)");
      return Value(-std::get<int64_t>(v));
    }
    if (std::holds_alternative<double>(v)) return Value(-std::get<double>(v));
    const auto& d = std::get<DecimalValue>(v);
    return Value(DecimalValue{-d.raw, d.scale});
  }

  static Value Kleene(BinaryOp op, const Value& a, const Value& b) {
    // Encode as 0 = FALSE, 1 = TRUE, 2 = NULL and apply the textbook tables.
    const auto code = [](const Value& v) { return IsNull(v) ? 2 : (std::get<bool>(v) ? 1 : 0); };
    const int x = code(a);
    const int y = code(b);
    int r = 2;
    if (op == BinaryOp::kAnd) {
      if (x == 0 || y == 0)
        r = 0;
      else if (x == 1 && y == 1)
        r = 1;
    } else {
      if (x == 1 || y == 1)
        r = 1;
      else if (x == 0 && y == 0)
        r = 0;
    }
    return r == 2 ? Value{} : Value(r == 1);
  }

  static arrow::Result<Value> Cmp(BinaryOp op, const Value& a, const Value& b) {
    if (IsNull(a) || IsNull(b)) return Value{};
    ARROW_ASSIGN_OR_RAISE(int c, Compare(a, b));
    switch (op) {
      case BinaryOp::kEq:
        return Value(c == 0);
      case BinaryOp::kNe:
        return Value(c != 0);
      case BinaryOp::kLt:
        return Value(c < 0);
      case BinaryOp::kLe:
        return Value(c <= 0);
      case BinaryOp::kGt:
        return Value(c > 0);
      default:
        return Value(c >= 0);
    }
  }

  arrow::Result<Value> Binary(const BoundExpr& e, const Row& row) const {
    ARROW_ASSIGN_OR_RAISE(Value a, Eval(*e.children[0], row));
    ARROW_ASSIGN_OR_RAISE(Value b, Eval(*e.children[1], row));
    if (e.op == BinaryOp::kAnd || e.op == BinaryOp::kOr) return Kleene(e.op, a, b);
    if (IsNull(a) || IsNull(b)) return Value{};
    if (IsComparison(e.op)) return Cmp(e.op, a, b);
    return ArithmeticValues(e, a, b);
  }
};

}  // namespace

arrow::Result<std::vector<Value>> ReferenceEvaluate(const BoundExpr& expr,
                                                    const std::vector<std::vector<Value>>& rows) {
  RowEvaluator evaluator;
  std::vector<Value> out;
  out.reserve(rows.size());
  for (const auto& row : rows) {
    ARROW_ASSIGN_OR_RAISE(Value v, evaluator.Eval(expr, row));
    out.push_back(std::move(v));
  }
  return out;
}

}  // namespace quarry::test
