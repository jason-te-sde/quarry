#ifndef QUARRY_TYPES_TYPE_RULES_H_
#define QUARRY_TYPES_TYPE_RULES_H_

#include <arrow/result.h>

#include <cstdint>
#include <string_view>

#include "types/logical_type.h"

namespace quarry {

enum class BinaryOp : uint8_t { kAdd, kSub, kMul, kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr };

// "+", "-", "*", "=", "<>", "<", "<=", ">", ">=", "AND", "OR".
const char* BinaryOpName(BinaryOp op);
bool IsComparison(BinaryOp op);
bool IsArithmetic(BinaryOp op);

// How an integer type behaves when it meets a DECIMAL: INTEGER is DECIMAL(10,0), BIGINT is
// DECIMAL(19,0). Any other type is returned unchanged.
LogicalType AsDecimalType(const LogicalType& t);

// The result type of l op r, following the rules DuckDB prints with typeof() (pinned in
// docs/sql.md by scripts/duckdb_types.py, explained in docs/design/0004). Invalid when the
// operator does not apply to the operand types.
//   - integers: the wider of the two (INTEGER op INTEGER is INTEGER)
//   - DOUBLE with any number: DOUBLE
//   - DECIMAL with DECIMAL or an integer (INTEGER is DECIMAL(10,0), BIGINT DECIMAL(19,0)):
//     add and subtract give scale max(s1,s2) and width max(p1-s1,p2-s2)+max(s1,s2)+1;
//     multiply gives scale s1+s2 and width p1+p2, which becomes 18 when both operands fit in 64
//     bits and the sum exceeds 18, and is capped at 38
//   - DATE +/- INTEGER: DATE
//   - comparisons: BOOLEAN when CommonComparisonType exists; AND and OR: BOOLEAN of BOOLEANs
arrow::Result<LogicalType> BinaryResultType(BinaryOp op, const LogicalType& l,
                                            const LogicalType& r);

// The type both operands are converted to before they are compared. Invalid when they cannot be
// compared (VARCHAR with a number, DATE with a number, ...).
arrow::Result<LogicalType> CommonComparisonType(const LogicalType& l, const LogicalType& r);

// The type of a decimal literal written as `digits` ("0.05" is DECIMAL(3,2), "123.4500" is
// DECIMAL(7,4)): the precision counts every digit written, the scale the digits after the point.
// `digits` must be an unsigned decimal literal; the precision is capped at 38.
LogicalType DecimalLiteralType(std::string_view digits);

enum class AggFunc : uint8_t { kSum, kAvg, kCount, kCountStar, kMin, kMax };

const char* AggFuncName(AggFunc func);

// SUM(DECIMAL(p,s)) is DECIMAL(38,s); SUM of an integer is DECIMAL(38,0) (DuckDB says HUGEINT,
// which quarry does not have; results are compared by value); SUM(DOUBLE) is DOUBLE; AVG of a
// number is DOUBLE; COUNT is BIGINT; MIN and MAX keep the argument type. Invalid for SUM or AVG of
// a non-number.
arrow::Result<LogicalType> AggregateResultType(AggFunc func, const LogicalType& arg);

}  // namespace quarry

#endif  // QUARRY_TYPES_TYPE_RULES_H_
