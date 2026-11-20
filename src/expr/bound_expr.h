#ifndef QUARRY_EXPR_BOUND_EXPR_H_
#define QUARRY_EXPR_BOUND_EXPR_H_

#include <arrow/result.h>
#include <arrow/scalar.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "types/logical_type.h"
#include "types/type_rules.h"

namespace quarry {

enum class ExprKind : uint8_t {
  kColumn,   // column_index
  kLiteral,  // literal
  kNegate,   // children[0]
  kNot,      // children[0]
  kBinary,   // op, children[0] op children[1]
  kBetween,  // children[0] BETWEEN children[1] AND children[2]
  kInList,   // children[0] IN (in_list)
  kIsNull,   // children[0] IS [NOT] NULL, negated for IS NOT NULL
  kCast,     // CAST(children[0] AS type)
};

struct BoundExpr;
using ExprPtr = std::unique_ptr<BoundExpr>;

// A typed expression tree. Every node knows its result type. The factory functions below are the
// way to build one: they compute the types and insert the casts, so that a tree they produce has
// operands of identical types wherever a kernel needs that (see Evaluate).
struct BoundExpr {
  ExprKind kind = ExprKind::kLiteral;
  LogicalType type;
  std::vector<ExprPtr> children;

  int column_index = -1;                                // kColumn
  std::shared_ptr<arrow::Scalar> literal;               // kLiteral
  BinaryOp op = BinaryOp::kAdd;                         // kBinary
  std::vector<std::shared_ptr<arrow::Scalar>> in_list;  // kInList
  bool negated = false;                                 // kIsNull: IS NOT NULL

  ExprPtr Clone() const;
  // A one-line, fully parenthesised rendering, for plans and failure messages:
  // ((#0 * (1.00 - #1)) >= CAST(#2 AS DECIMAL(18,2))).
  std::string ToString() const;
};

ExprPtr MakeColumn(int index, const LogicalType& type);

// A literal from an Arrow scalar of one of the engine's types (a NULL scalar included, which is
// typed). A decimal128 scalar with precision <= 18 becomes decimal64. Invalid for any other type.
arrow::Result<ExprPtr> MakeLiteral(std::shared_ptr<arrow::Scalar> scalar);
// A literal such as "0.05" or "12": the type is DecimalLiteralType(digits).
arrow::Result<ExprPtr> MakeDecimalLiteral(std::string_view digits);

arrow::Result<ExprPtr> MakeNegate(ExprPtr child);
arrow::Result<ExprPtr> MakeNot(ExprPtr child);
arrow::Result<ExprPtr> MakeIsNull(ExprPtr child, bool negated);
arrow::Result<ExprPtr> MakeCast(ExprPtr child, const LogicalType& to);
// The type is BinaryResultType(op, l, r). Comparisons cast both operands to the common type;
// arithmetic casts the operands to the type the kernel computes in (DECIMAL add and subtract in
// the result type, integers and decimals meeting a DOUBLE as DOUBLE, an INTEGER meeting a BIGINT as
// BIGINT, DECIMAL multiply operands widened to 128 bits when the result is).
arrow::Result<ExprPtr> MakeBinary(BinaryOp op, ExprPtr l, ExprPtr r);
// x BETWEEN lo AND hi, both bounds inclusive; the three are cast to one common type.
arrow::Result<ExprPtr> MakeBetween(ExprPtr x, ExprPtr lo, ExprPtr hi);
// x IN (values...); the operand and the values are cast to one common type. NOT IN is
// MakeNot(MakeInList(...)). The list must not be empty.
arrow::Result<ExprPtr> MakeInList(ExprPtr x,
                                  const std::vector<std::shared_ptr<arrow::Scalar>>& values);

}  // namespace quarry

#endif  // QUARRY_EXPR_BOUND_EXPR_H_
