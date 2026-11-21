#ifndef QUARRY_EXPR_EVAL_H_
#define QUARRY_EXPR_EVAL_H_

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/scalar.h>

#include <memory>

#include "expr/bound_expr.h"
#include "types/logical_type.h"
#include "vector/batch.h"
#include "vector/selection.h"

namespace quarry {

// Evaluates the expression over the live rows of the batch, column at a time. The result has
// active_rows() rows, in selection order, and the Arrow type expr.type.ToArrow(). Arithmetic that
// overflows (an integer or a DECIMAL beyond its precision) is an Invalid status mentioning
// "overflow"; rows that are NULL in an operand never overflow. A column of the batch must have the
// Arrow type of the BoundExpr's column type, except that a decimal128 column of precision <= 18
// (what the Parquet reader returns) is accepted for DECIMAL(p, s).
arrow::Result<std::shared_ptr<arrow::Array>> Evaluate(const BoundExpr& expr, const Batch& batch,
                                                      arrow::MemoryPool* pool);

// The rows, as indices into the batch (not into the selection), where the boolean expression is
// TRUE. NULL and FALSE drop the row.
arrow::Result<SelectionVector> EvaluatePredicate(const BoundExpr& expr, const Batch& batch,
                                                 arrow::MemoryPool* pool);

// A scalar converted to another type with the same rules as CAST.
arrow::Result<std::shared_ptr<arrow::Scalar>> CastScalar(const arrow::Scalar& scalar,
                                                         const LogicalType& to,
                                                         arrow::MemoryPool* pool);

}  // namespace quarry

#endif  // QUARRY_EXPR_EVAL_H_
