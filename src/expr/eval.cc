#include "expr/eval.h"

#include <arrow/array/util.h>
#include <arrow/status.h>

#include <algorithm>
#include <span>
#include <vector>

#include "common/flaws.h"
#include "expr/eval_internal.h"
#include "vector/gather.h"

namespace quarry {
namespace {

using eval_internal::ArrayPtr;

// Evaluates one expression tree over the live rows of one batch. The live rows are either all rows
// of the batch (no selection) or the selection's indices; every intermediate array has one slot
// per live row, so the kernels never see the selection.
class Evaluator {
 public:
  Evaluator(const Batch& batch, arrow::MemoryPool* pool)
      : batch_(batch), pool_(pool), rows_(batch.active_rows()) {}

  arrow::Result<ArrayPtr> Eval(const BoundExpr& expr) {
    switch (expr.kind) {
      case ExprKind::kColumn:
        return EvalColumn(expr);
      case ExprKind::kLiteral:
        return EvalLiteral(expr);
      case ExprKind::kNegate: {
        ARROW_ASSIGN_OR_RAISE(auto child, Eval(*expr.children[0]));
        return eval_internal::EvalNegate(*child, expr.children[0]->type, pool_);
      }
      case ExprKind::kNot: {
        ARROW_ASSIGN_OR_RAISE(auto child, Eval(*expr.children[0]));
        return eval_internal::EvalNot(*child, pool_);
      }
      case ExprKind::kBinary:
        return EvalBinary(expr);
      case ExprKind::kBetween:
        return EvalBetween(expr);
      case ExprKind::kInList:
        return EvalInList(expr);
      case ExprKind::kIsNull: {
        ARROW_ASSIGN_OR_RAISE(auto child, Eval(*expr.children[0]));
        return eval_internal::EvalIsNull(*child, expr.negated, pool_);
      }
      case ExprKind::kCast: {
        ARROW_ASSIGN_OR_RAISE(auto child, Eval(*expr.children[0]));
        return eval_internal::EvalCast(*child, expr.children[0]->type, expr.type, pool_);
      }
    }
    return arrow::Status::Invalid("unknown expression kind");
  }

 private:
  arrow::Result<ArrayPtr> EvalColumn(const BoundExpr& expr) {
    if (expr.column_index < 0 || static_cast<size_t>(expr.column_index) >= batch_.columns.size()) {
      return arrow::Status::Invalid("column #", expr.column_index,
                                    " is out of range: the batch has ", batch_.columns.size(),
                                    " columns");
    }
    const ArrayPtr& column = batch_.columns[static_cast<size_t>(expr.column_index)];
    if (column->length() != batch_.length) {
      return arrow::Status::Invalid("column #", expr.column_index, " has ", column->length(),
                                    " rows, the batch has ", batch_.length);
    }
    ArrayPtr values = column;
    if (batch_.selection) {
      if (batch_.selection->batch_length() != batch_.length) {
        return arrow::Status::Invalid("the selection vector was made for a batch of ",
                                      batch_.selection->batch_length(), " rows, the batch has ",
                                      batch_.length);
      }
      std::span<const uint32_t> indices = batch_.selection->indices();
      std::vector<uint32_t> shifted;
      if constexpr (kFlawSelectionOffByOne) {
        // The defect: the last selected row is read one row too far (kept inside the array).
        if (!indices.empty()) {
          shifted.assign(indices.begin(), indices.end());
          if (static_cast<int64_t>(shifted.back()) + 1 < batch_.length) {
            ++shifted.back();
          }
          indices = shifted;
        }
      }
      ARROW_ASSIGN_OR_RAISE(values, Gather(*column, indices, pool_));
    }
    // The Parquet reader returns decimal128 even for DECIMAL(p <= 18); the kernels work on the
    // decimal64 of the engine's type.
    if (expr.type.is_decimal64() && values->type_id() == arrow::Type::DECIMAL128) {
      ARROW_ASSIGN_OR_RAISE(values, eval_internal::NarrowDecimal128(*values, pool_));
    }
    ARROW_RETURN_NOT_OK(eval_internal::CheckArrayType(
        *values, expr.type, ("column #" + std::to_string(expr.column_index)).c_str()));
    return values;
  }

  arrow::Result<ArrayPtr> EvalLiteral(const BoundExpr& expr) {
    return arrow::MakeArrayFromScalar(*expr.literal, rows_, pool_);
  }

  arrow::Result<ArrayPtr> EvalBinary(const BoundExpr& expr) {
    const BoundExpr& left = *expr.children[0];
    const BoundExpr& right = *expr.children[1];
    ARROW_ASSIGN_OR_RAISE(auto l, Eval(left));
    ARROW_ASSIGN_OR_RAISE(auto r, Eval(right));
    if (IsArithmetic(expr.op)) {
      return eval_internal::EvalArithmetic(expr.op, *l, left.type, *r, right.type, expr.type,
                                           pool_);
    }
    if (IsComparison(expr.op)) {
      return eval_internal::EvalComparison(expr.op, *l, *r, left.type, pool_);
    }
    return eval_internal::EvalLogical(expr.op, *l, *r, pool_);
  }

  arrow::Result<ArrayPtr> EvalBetween(const BoundExpr& expr) {
    ARROW_ASSIGN_OR_RAISE(auto x, Eval(*expr.children[0]));
    ARROW_ASSIGN_OR_RAISE(auto lo, Eval(*expr.children[1]));
    ARROW_ASSIGN_OR_RAISE(auto hi, Eval(*expr.children[2]));
    const LogicalType& type = expr.children[0]->type;
    ARROW_ASSIGN_OR_RAISE(auto ge,
                          eval_internal::EvalComparison(BinaryOp::kGe, *x, *lo, type, pool_));
    ARROW_ASSIGN_OR_RAISE(auto le,
                          eval_internal::EvalComparison(BinaryOp::kLe, *x, *hi, type, pool_));
    return eval_internal::EvalLogical(BinaryOp::kAnd, *ge, *le, pool_);
  }

  arrow::Result<ArrayPtr> EvalInList(const BoundExpr& expr) {
    ARROW_ASSIGN_OR_RAISE(auto x, Eval(*expr.children[0]));
    const LogicalType& type = expr.children[0]->type;
    ArrayPtr any;
    for (const auto& value : expr.in_list) {
      ARROW_ASSIGN_OR_RAISE(auto constant, arrow::MakeArrayFromScalar(*value, rows_, pool_));
      ARROW_ASSIGN_OR_RAISE(
          auto equal, eval_internal::EvalComparison(BinaryOp::kEq, *x, *constant, type, pool_));
      if (any == nullptr) {
        any = equal;
      } else {
        ARROW_ASSIGN_OR_RAISE(any, eval_internal::EvalLogical(BinaryOp::kOr, *any, *equal, pool_));
      }
    }
    if (any == nullptr) {
      return arrow::Status::Invalid("IN list is empty");
    }
    return any;
  }

  const Batch& batch_;
  arrow::MemoryPool* pool_;
  int64_t rows_;
};

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> Evaluate(const BoundExpr& expr, const Batch& batch,
                                                      arrow::MemoryPool* pool) {
  return Evaluator(batch, pool).Eval(expr);
}

arrow::Result<SelectionVector> EvaluatePredicate(const BoundExpr& expr, const Batch& batch,
                                                 arrow::MemoryPool* pool) {
  if (expr.type.id != TypeId::kBoolean) {
    return arrow::Status::Invalid("a predicate must be BOOLEAN but is ", expr.type.ToString());
  }
  if (batch.length > static_cast<int64_t>(UINT32_MAX)) {
    return arrow::Status::Invalid("a batch of ", batch.length, " rows is too long for a selection");
  }
  ARROW_ASSIGN_OR_RAISE(auto result, Evaluate(expr, batch, pool));
  const int64_t n = result->length();
  const uint8_t* bits = eval_internal::ValueBytes(*result);
  const uint8_t* valid = result->null_count() > 0 ? result->null_bitmap_data() : nullptr;
  std::vector<uint32_t> hits;
  std::span<const uint32_t> live;
  if (batch.selection) {
    live = batch.selection->indices();
  }
  for (int64_t i = 0; i < n; ++i) {
    const int64_t bit = result->offset() + i;
    if ((valid == nullptr || eval_internal::BitAt(valid, bit)) && eval_internal::BitAt(bits, bit)) {
      hits.push_back(batch.selection ? live[static_cast<size_t>(i)] : static_cast<uint32_t>(i));
    }
  }
  return SelectionVector::Make(std::move(hits), batch.length);
}

arrow::Result<std::shared_ptr<arrow::Scalar>> CastScalar(const arrow::Scalar& scalar,
                                                         const LogicalType& to,
                                                         arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(auto from, LogicalType::FromArrow(*scalar.type));
  ARROW_ASSIGN_OR_RAISE(auto array, arrow::MakeArrayFromScalar(scalar, 1, pool));
  if (from.is_decimal64() && array->type_id() == arrow::Type::DECIMAL128) {
    ARROW_ASSIGN_OR_RAISE(array, eval_internal::NarrowDecimal128(*array, pool));
  }
  ARROW_ASSIGN_OR_RAISE(auto cast, eval_internal::EvalCast(*array, from, to, pool));
  return cast->GetScalar(0);
}

}  // namespace quarry
