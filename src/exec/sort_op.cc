#include <arrow/status.h>

#include <algorithm>
#include <numeric>
#include <vector>

#include "exec/operator_util.h"
#include "exec/operators.h"
#include "exec/row_comparator.h"
#include "expr/eval.h"
#include "vector/gather.h"

namespace quarry {
namespace {

// Reads its whole input, sorts it (stably: rows that tie keep their input order) and hands it out
// in batches.
class SortOperator final : public Operator {
 public:
  SortOperator(const PhysicalSort& node, std::unique_ptr<Operator> input)
      : node_(node), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    reservation_ =
        std::make_unique<Reservation>(*context.budget, ReservationOwner("Sort", node_.id));
    charged_ = std::make_unique<ChargedBytes>(reservation_.get());
    return input_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (!sorted_) {
      ARROW_RETURN_NOT_OK(SortInput());
      sorted_ = true;
    }
    if (position_ >= static_cast<int64_t>(order_.size())) return std::optional<Batch>();
    const int64_t count =
        std::min<int64_t>(context_->batch_size, static_cast<int64_t>(order_.size()) - position_);
    const std::span<const uint32_t> slice(order_.data() + position_, static_cast<size_t>(count));
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const auto& column : columns_) {
      ARROW_ASSIGN_OR_RAISE(auto gathered, Gather(*column, slice, context_->pool));
      columns.push_back(std::move(gathered));
    }
    position_ += count;
    return std::optional<Batch>(MakeBatch(std::move(columns), count));
  }

  void Close() override { input_->Close(); }

  void FillStats(OperatorStats* stats) const override {
    if (reservation_ != nullptr) stats->peak_state_bytes = reservation_->peak();
  }

 private:
  arrow::Status SortInput() {
    const size_t width = node_.output_types.size();
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> parts(width);
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> key_parts(node_.keys.size());
    int64_t bytes = 0;
    int64_t rows = 0;
    for (;;) {
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> next, input_->Next());
      if (!next.has_value()) break;
      ARROW_ASSIGN_OR_RAISE(Batch batch, next->Compact(context_->pool));
      for (size_t c = 0; c < width; ++c) parts[c].push_back(batch.columns[c]);
      for (size_t k = 0; k < node_.keys.size(); ++k) {
        const BoundExpr& expr = *node_.keys[k].expr;
        if (expr.kind == ExprKind::kColumn) continue;  // the column itself is the key
        ARROW_ASSIGN_OR_RAISE(auto key, Evaluate(expr, batch, context_->pool));
        bytes += ArrayBytes(*key);
        key_parts[k].push_back(std::move(key));
      }
      bytes += BatchBytes(batch);
      rows += batch.length;
      ARROW_RETURN_NOT_OK(charged_->Set(bytes));
    }
    if (rows > static_cast<int64_t>(UINT32_MAX)) {
      return arrow::Status::Invalid("a sort is limited to ", UINT32_MAX, " rows");
    }
    columns_.clear();
    for (size_t c = 0; c < width; ++c) {
      ARROW_ASSIGN_OR_RAISE(
          auto column,
          ConcatenateArrays(parts[c], node_.output_types[c].ToArrow(), context_->pool));
      columns_.push_back(std::move(column));
    }
    std::vector<SortColumn> sort_columns;
    for (size_t k = 0; k < node_.keys.size(); ++k) {
      const SortKey& key = node_.keys[k];
      SortColumn column;
      if (key.expr->kind == ExprKind::kColumn) {
        column.array = columns_[static_cast<size_t>(key.expr->column_index)];
      } else {
        ARROW_ASSIGN_OR_RAISE(
            column.array,
            ConcatenateArrays(key_parts[k], key.expr->type.ToArrow(), context_->pool));
      }
      column.ascending = key.ascending;
      column.nulls_first = key.nulls_first;
      sort_columns.push_back(std::move(column));
    }
    ARROW_ASSIGN_OR_RAISE(RowComparator comparator, RowComparator::Make(sort_columns));
    order_.resize(static_cast<size_t>(rows));
    std::iota(order_.begin(), order_.end(), 0u);
    ARROW_RETURN_NOT_OK(charged_->Set(bytes + rows * static_cast<int64_t>(sizeof(uint32_t))));
    std::stable_sort(order_.begin(), order_.end(),
                     [&](uint32_t a, uint32_t b) { return comparator.Compare(a, b) < 0; });
    return arrow::Status::OK();
  }

  const PhysicalSort& node_;
  std::unique_ptr<Operator> input_;
  ExecContext* context_ = nullptr;
  std::unique_ptr<Reservation> reservation_;
  std::unique_ptr<ChargedBytes> charged_;
  bool sorted_ = false;
  std::vector<std::shared_ptr<arrow::Array>> columns_;
  std::vector<uint32_t> order_;
  int64_t position_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeSortOperator(const PhysicalSort& node,
                                           std::unique_ptr<Operator> input) {
  return std::make_unique<SortOperator>(node, std::move(input));
}

}  // namespace quarry
