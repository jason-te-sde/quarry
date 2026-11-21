#include "expr/bound_expr.h"

#include <arrow/memory_pool.h>
#include <arrow/status.h>
#include <arrow/util/decimal.h>

#include <algorithm>
#include <utility>

#include "expr/eval.h"
#include "types/date.h"
#include "types/decimal.h"

namespace quarry {
namespace {

ExprPtr NewNode(ExprKind kind, const LogicalType& type) {
  auto node = std::make_unique<BoundExpr>();
  node->kind = kind;
  node->type = type;
  return node;
}

bool IsNumber(const LogicalType& t) {
  return t.is_integer() || t.is_decimal() || t.id == TypeId::kDouble;
}

// The casts that EvalCast implements.
bool CastSupported(const LogicalType& from, const LogicalType& to) {
  if (from == to) {
    return true;
  }
  switch (to.id) {
    case TypeId::kInt64:
      return from.id == TypeId::kInt32;
    case TypeId::kDouble:
    case TypeId::kDecimal:
      return from.is_integer() || from.is_decimal();
    default:
      return false;
  }
}

arrow::Result<ExprPtr> Coerce(ExprPtr expr, const LogicalType& to) {
  if (expr->type == to) {
    return expr;
  }
  return MakeCast(std::move(expr), to);
}

arrow::Result<LogicalType> CommonOf(const std::vector<const LogicalType*>& types) {
  LogicalType common = *types.front();
  for (size_t i = 1; i < types.size(); ++i) {
    ARROW_ASSIGN_OR_RAISE(common, CommonComparisonType(common, *types[i]));
  }
  return common;
}

std::string ScalarToString(const arrow::Scalar& scalar) {
  if (!scalar.is_valid) {
    return "NULL";
  }
  if (scalar.type->id() == arrow::Type::STRING) {
    return "'" + scalar.ToString() + "'";
  }
  if (scalar.type->id() == arrow::Type::DATE32) {
    return "DATE '" + FormatDate(static_cast<const arrow::Date32Scalar&>(scalar).value) + "'";
  }
  return scalar.ToString();
}

}  // namespace

ExprPtr BoundExpr::Clone() const {
  auto copy = NewNode(kind, type);
  copy->column_index = column_index;
  copy->literal = literal;
  copy->op = op;
  copy->in_list = in_list;
  copy->negated = negated;
  copy->children.reserve(children.size());
  for (const auto& child : children) {
    copy->children.push_back(child->Clone());
  }
  return copy;
}

std::string BoundExpr::ToString() const {
  switch (kind) {
    case ExprKind::kColumn:
      return "#" + std::to_string(column_index);
    case ExprKind::kLiteral:
      return ScalarToString(*literal);
    case ExprKind::kNegate:
      return "-(" + children[0]->ToString() + ")";
    case ExprKind::kNot:
      return "NOT (" + children[0]->ToString() + ")";
    case ExprKind::kBinary:
      return "(" + children[0]->ToString() + " " + BinaryOpName(op) + " " +
             children[1]->ToString() + ")";
    case ExprKind::kBetween:
      return "(" + children[0]->ToString() + " BETWEEN " + children[1]->ToString() + " AND " +
             children[2]->ToString() + ")";
    case ExprKind::kInList: {
      std::string out = "(" + children[0]->ToString() + " IN (";
      for (size_t i = 0; i < in_list.size(); ++i) {
        out += (i == 0 ? "" : ", ") + ScalarToString(*in_list[i]);
      }
      return out + "))";
    }
    case ExprKind::kIsNull:
      return "(" + children[0]->ToString() + (negated ? " IS NOT NULL)" : " IS NULL)");
    case ExprKind::kCast:
      return "CAST(" + children[0]->ToString() + " AS " + type.ToString() + ")";
  }
  return "?";
}

ExprPtr MakeColumn(int index, const LogicalType& type) {
  auto node = NewNode(ExprKind::kColumn, type);
  node->column_index = index;
  return node;
}

arrow::Result<ExprPtr> MakeLiteral(std::shared_ptr<arrow::Scalar> scalar) {
  if (scalar == nullptr) {
    return arrow::Status::Invalid("a literal needs a scalar");
  }
  ARROW_ASSIGN_OR_RAISE(auto type, LogicalType::FromArrow(*scalar->type));
  if (type.is_decimal64() && scalar->type->id() == arrow::Type::DECIMAL128) {
    if (!scalar->is_valid) {
      scalar = arrow::MakeNullScalar(type.ToArrow());
    } else {
      const auto& wide = static_cast<const arrow::Decimal128Scalar&>(*scalar);
      scalar = std::make_shared<arrow::Decimal64Scalar>(
          arrow::Decimal64(static_cast<int64_t>(wide.value.low_bits())), type.ToArrow());
    }
  }
  auto node = NewNode(ExprKind::kLiteral, type);
  node->literal = std::move(scalar);
  return node;
}

arrow::Result<ExprPtr> MakeDecimalLiteral(std::string_view digits) {
  int scale = 0;
  ARROW_ASSIGN_OR_RAISE(const Int128 raw, ParseDecimalDigits(digits, &scale));
  const LogicalType type = DecimalLiteralType(digits);
  std::shared_ptr<arrow::Scalar> scalar;
  if (type.is_decimal64()) {
    scalar = std::make_shared<arrow::Decimal64Scalar>(arrow::Decimal64(static_cast<int64_t>(raw)),
                                                      type.ToArrow());
  } else {
    scalar = std::make_shared<arrow::Decimal128Scalar>(
        arrow::Decimal128(static_cast<int64_t>(raw >> 64), static_cast<uint64_t>(raw)),
        type.ToArrow());
  }
  return MakeLiteral(std::move(scalar));
}

arrow::Result<ExprPtr> MakeNegate(ExprPtr child) {
  if (!IsNumber(child->type)) {
    return arrow::Status::Invalid("unary minus cannot be applied to ", child->type.ToString());
  }
  auto node = NewNode(ExprKind::kNegate, child->type);
  node->children.push_back(std::move(child));
  return node;
}

arrow::Result<ExprPtr> MakeNot(ExprPtr child) {
  if (child->type.id != TypeId::kBoolean) {
    return arrow::Status::Invalid("NOT cannot be applied to ", child->type.ToString());
  }
  auto node = NewNode(ExprKind::kNot, LogicalType::Boolean());
  node->children.push_back(std::move(child));
  return node;
}

arrow::Result<ExprPtr> MakeIsNull(ExprPtr child, bool negated) {
  auto node = NewNode(ExprKind::kIsNull, LogicalType::Boolean());
  node->negated = negated;
  node->children.push_back(std::move(child));
  return node;
}

arrow::Result<ExprPtr> MakeCast(ExprPtr child, const LogicalType& to) {
  if (!CastSupported(child->type, to)) {
    return arrow::Status::Invalid("cannot cast ", child->type.ToString(), " to ", to.ToString());
  }
  if (child->type == to) {
    return child;
  }
  auto node = NewNode(ExprKind::kCast, to);
  node->children.push_back(std::move(child));
  return node;
}

arrow::Result<ExprPtr> MakeBinary(BinaryOp op, ExprPtr l, ExprPtr r) {
  ARROW_ASSIGN_OR_RAISE(const LogicalType result, BinaryResultType(op, l->type, r->type));
  if (IsComparison(op)) {
    ARROW_ASSIGN_OR_RAISE(const LogicalType common, CommonComparisonType(l->type, r->type));
    ARROW_ASSIGN_OR_RAISE(l, Coerce(std::move(l), common));
    ARROW_ASSIGN_OR_RAISE(r, Coerce(std::move(r), common));
  } else if (IsArithmetic(op) && result.id != TypeId::kDate) {
    if (result.is_decimal() && op == BinaryOp::kMul) {
      // The product is computed on the operands' own scales; only the storage width follows the
      // result.
      const auto operand_type = [&](const LogicalType& t) {
        const LogicalType d = AsDecimalType(t);
        return result.is_decimal64()
                   ? d
                   : LogicalType::Decimal(std::max<int>(d.precision, kMaxInt64DecimalPrecision + 1),
                                          d.scale);
      };
      const LogicalType left_type = operand_type(l->type);
      const LogicalType right_type = operand_type(r->type);
      ARROW_ASSIGN_OR_RAISE(l, Coerce(std::move(l), left_type));
      ARROW_ASSIGN_OR_RAISE(r, Coerce(std::move(r), right_type));
    } else {
      ARROW_ASSIGN_OR_RAISE(l, Coerce(std::move(l), result));
      ARROW_ASSIGN_OR_RAISE(r, Coerce(std::move(r), result));
    }
  }
  auto node = NewNode(ExprKind::kBinary, result);
  node->op = op;
  node->children.push_back(std::move(l));
  node->children.push_back(std::move(r));
  return node;
}

arrow::Result<ExprPtr> MakeBetween(ExprPtr x, ExprPtr lo, ExprPtr hi) {
  ARROW_ASSIGN_OR_RAISE(const LogicalType common, CommonOf({&x->type, &lo->type, &hi->type}));
  ARROW_ASSIGN_OR_RAISE(x, Coerce(std::move(x), common));
  ARROW_ASSIGN_OR_RAISE(lo, Coerce(std::move(lo), common));
  ARROW_ASSIGN_OR_RAISE(hi, Coerce(std::move(hi), common));
  auto node = NewNode(ExprKind::kBetween, LogicalType::Boolean());
  node->children.push_back(std::move(x));
  node->children.push_back(std::move(lo));
  node->children.push_back(std::move(hi));
  return node;
}

arrow::Result<ExprPtr> MakeInList(ExprPtr x,
                                  const std::vector<std::shared_ptr<arrow::Scalar>>& values) {
  if (values.empty()) {
    return arrow::Status::Invalid("an IN list needs at least one value");
  }
  std::vector<LogicalType> types{x->type};
  for (const auto& value : values) {
    if (value == nullptr) {
      return arrow::Status::Invalid("an IN list value is missing");
    }
    ARROW_ASSIGN_OR_RAISE(auto type, LogicalType::FromArrow(*value->type));
    types.push_back(type);
  }
  std::vector<const LogicalType*> pointers;
  pointers.reserve(types.size());
  for (const auto& type : types) {
    pointers.push_back(&type);
  }
  ARROW_ASSIGN_OR_RAISE(const LogicalType common, CommonOf(pointers));
  ARROW_ASSIGN_OR_RAISE(x, Coerce(std::move(x), common));
  auto node = NewNode(ExprKind::kInList, LogicalType::Boolean());
  for (const auto& value : values) {
    ARROW_ASSIGN_OR_RAISE(auto cast, CastScalar(*value, common, arrow::default_memory_pool()));
    node->in_list.push_back(std::move(cast));
  }
  node->children.push_back(std::move(x));
  return node;
}

}  // namespace quarry
