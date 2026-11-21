#include "parser/ast.h"

namespace quarry::ast {

const char* BinOpName(BinOp op) {
  switch (op) {
    case BinOp::kAdd:
      return "+";
    case BinOp::kSub:
      return "-";
    case BinOp::kMul:
      return "*";
    case BinOp::kEq:
      return "=";
    case BinOp::kNe:
      return "<>";
    case BinOp::kLt:
      return "<";
    case BinOp::kLe:
      return "<=";
    case BinOp::kGt:
      return ">";
    case BinOp::kGe:
      return ">=";
    case BinOp::kAnd:
      return "AND";
    case BinOp::kOr:
      return "OR";
  }
  return "?";
}

namespace {

std::string Name(const std::string& name, bool quoted) {
  return quoted ? "\"" + name + "\"" : name;
}

const char* UnitName(IntervalUnit unit) {
  switch (unit) {
    case IntervalUnit::kDay:
      return "day";
    case IntervalUnit::kMonth:
      return "month";
    case IntervalUnit::kYear:
      return "year";
  }
  return "?";
}

}  // namespace

std::string Expr::ToString() const {
  std::string out;
  const auto with_children = [&](const std::string& head) {
    out = "(" + head;
    for (const ExprPtr& child : children) out += " " + child->ToString();
    out += ")";
    return out;
  };
  switch (kind) {
    case ExprKind::kInteger:
    case ExprKind::kDecimal:
      return text;
    case ExprKind::kString:
      return "'" + text + "'";
    case ExprKind::kBoolean:
      return boolean_value ? "true" : "false";
    case ExprKind::kNull:
      return "null";
    case ExprKind::kDate:
      return "(date '" + text + "')";
    case ExprKind::kInterval:
      return "(interval " + std::to_string(interval_count) + " " + UnitName(interval_unit) + ")";
    case ExprKind::kColumn:
      return has_qualifier ? Name(qualifier, qualifier_quoted) + "." + Name(text, quoted)
                           : Name(text, quoted);
    case ExprKind::kUnary:
      return with_children(unary_op == UnaryOp::kNegate ? "neg" : "not");
    case ExprKind::kBinary:
      return with_children(BinOpName(binary_op));
    case ExprKind::kBetween:
      return with_children(negated ? "not-between" : "between");
    case ExprKind::kInList:
      return with_children(negated ? "not-in" : "in");
    case ExprKind::kIsNull:
      return with_children(negated ? "is-not-null" : "is-null");
    case ExprKind::kFunction:
      if (star) return "(" + text + " *)";
      return with_children(text);
  }
  return out;
}

}  // namespace quarry::ast
