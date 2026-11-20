#ifndef QUARRY_TESTS_SUPPORT_REFERENCE_EVAL_H_
#define QUARRY_TESTS_SUPPORT_REFERENCE_EVAL_H_

#include <arrow/result.h>

#include <vector>

#include "expr/bound_expr.h"
#include "support/value.h"

namespace quarry::test {

// Evaluates the expression one row at a time over `rows` (rows[r][c] is column c of row r). It is
// written without reference to src/expr/eval*.cc: scalar Values, no arrays, no validity bitmaps,
// decimal arithmetic in unsigned and widened integers with its own powers of ten. Both operands
// of every operator are always evaluated (no short circuit), exactly like the column kernels, so
// the two agree on whether an overflow happens. An overflow is an Invalid status.
arrow::Result<std::vector<Value>> ReferenceEvaluate(const BoundExpr& expr,
                                                    const std::vector<std::vector<Value>>& rows);

}  // namespace quarry::test

#endif  // QUARRY_TESTS_SUPPORT_REFERENCE_EVAL_H_
