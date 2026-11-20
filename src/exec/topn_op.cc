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

// The first n rows of the sorted input, without sorting all of it. Batches are buffered next to the
// best n rows so far; whenever the buffer is large, the best n of everything are picked again. Rows
// that tie on every sort key are ordered by arrival, so the result equals a stable sort followed by
// LIMIT n.
class TopNOperator final : public Operator {
 public:
  TopNOperator(const PhysicalTopN& node, std::unique_ptr<Operator> input)
      : node_(node), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    reservation_ =
        std::make_unique<Reservation>(*context.budget, ReservationOwner("TopN", node_.id));
    charged_ = std::make_unique<ChargedBytes>(reservation_.get());
    return input_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (!done_) {
      ARROW_RETURN_NOT_OK(Consume());
      done_ = true;
    }
    const int64_t total = best_rows_;
    if (position_ >= total) return std::optional<Batch>();
    const int64_t count = std::min<int64_t>(context_->batch_size, total - position_);
    std::vector<uint32_t> slice(static_cast<size_t>(count));
    std::iota(slice.begin(), slice.end(), static_cast<uint32_t>(position_));
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const auto& column : best_) {
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
  struct Pending {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    std::vector<std::shared_ptr<arrow::Array>> keys;  // evaluated key expressions, by key
    int64_t rows = 0;
  };

  arrow::Status Consume() {
    if (node_.n <= 0) return arrow::Status::OK();  // LIMIT 0 reads nothing
    const size_t width = node_.output_types.size();
    best_.clear();
    for (size_t c = 0; c < width; ++c) {
      ARROW_ASSIGN_OR_RAISE(auto empty,
                            ConcatenateArrays({}, node_.output_types[c].ToArrow(), context_->pool));
      best_.push_back(std::move(empty));
    }
    best_keys_.assign(node_.keys.size(), nullptr);
    for (size_t k = 0; k < node_.keys.size(); ++k) {
      if (node_.keys[k].expr->kind != ExprKind::kColumn) {
        ARROW_ASSIGN_OR_RAISE(
            best_keys_[k],
            ConcatenateArrays({}, node_.keys[k].expr->type.ToArrow(), context_->pool));
      }
    }
    const int64_t reduce_at = std::max<int64_t>(node_.n, context_->batch_size) * 2;
    std::vector<Pending> pending;
    int64_t pending_rows = 0;
    int64_t pending_bytes = 0;
    for (;;) {
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> next, input_->Next());
      if (!next.has_value()) break;
      ARROW_ASSIGN_OR_RAISE(Batch batch, next->Compact(context_->pool));
      Pending entry;
      entry.rows = batch.length;
      entry.keys.assign(node_.keys.size(), nullptr);
      for (size_t k = 0; k < node_.keys.size(); ++k) {
        const BoundExpr& expr = *node_.keys[k].expr;
        if (expr.kind == ExprKind::kColumn) continue;
        ARROW_ASSIGN_OR_RAISE(entry.keys[k], Evaluate(expr, batch, context_->pool));
        pending_bytes += ArrayBytes(*entry.keys[k]);
      }
      pending_bytes += BatchBytes(batch);
      entry.columns = std::move(batch.columns);
      pending_rows += entry.rows;
      pending.push_back(std::move(entry));
      ARROW_RETURN_NOT_OK(charged_->Set(BestBytes() + pending_bytes));
      if (pending_rows >= reduce_at) {
        ARROW_RETURN_NOT_OK(Reduce(&pending));
        pending_rows = 0;
        pending_bytes = 0;
        ARROW_RETURN_NOT_OK(charged_->Set(BestBytes()));
      }
    }
    ARROW_RETURN_NOT_OK(Reduce(&pending));
    return charged_->Set(BestBytes());
  }

  int64_t BestBytes() const {
    int64_t bytes = static_cast<int64_t>(best_seq_.size() * sizeof(int64_t));
    for (const auto& column : best_) bytes += ArrayBytes(*column);
    for (const auto& key : best_keys_) {
      if (key != nullptr) bytes += ArrayBytes(*key);
    }
    return bytes;
  }

  // Picks the best n rows of (the current best + everything pending) and makes them the new best.
  arrow::Status Reduce(std::vector<Pending>* pending) {
    if (pending->empty()) return arrow::Status::OK();
    const size_t width = best_.size();
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> parts(width);
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> key_parts(node_.keys.size());
    for (size_t c = 0; c < width; ++c) parts[c].push_back(best_[c]);
    for (size_t k = 0; k < node_.keys.size(); ++k) {
      if (best_keys_[k] != nullptr) key_parts[k].push_back(best_keys_[k]);
    }
    std::vector<int64_t> seq = best_seq_;
    for (const Pending& entry : *pending) {
      for (size_t c = 0; c < width; ++c) parts[c].push_back(entry.columns[c]);
      for (size_t k = 0; k < node_.keys.size(); ++k) {
        if (entry.keys[k] != nullptr) key_parts[k].push_back(entry.keys[k]);
      }
      for (int64_t i = 0; i < entry.rows; ++i) seq.push_back(next_seq_ + i);
      next_seq_ += entry.rows;
    }
    pending->clear();

    std::vector<std::shared_ptr<arrow::Array>> all(width);
    for (size_t c = 0; c < width; ++c) {
      ARROW_ASSIGN_OR_RAISE(
          all[c], ConcatenateArrays(parts[c], node_.output_types[c].ToArrow(), context_->pool));
    }
    std::vector<std::shared_ptr<arrow::Array>> all_keys(node_.keys.size());
    std::vector<SortColumn> sort_columns;
    for (size_t k = 0; k < node_.keys.size(); ++k) {
      const SortKey& key = node_.keys[k];
      SortColumn column;
      if (key.expr->kind == ExprKind::kColumn) {
        column.array = all[static_cast<size_t>(key.expr->column_index)];
      } else {
        ARROW_ASSIGN_OR_RAISE(
            all_keys[k], ConcatenateArrays(key_parts[k], key.expr->type.ToArrow(), context_->pool));
        column.array = all_keys[k];
      }
      column.ascending = key.ascending;
      column.nulls_first = key.nulls_first;
      sort_columns.push_back(std::move(column));
    }
    ARROW_ASSIGN_OR_RAISE(RowComparator comparator, RowComparator::Make(std::move(sort_columns)));

    const int64_t rows = all.empty() ? static_cast<int64_t>(seq.size()) : all[0]->length();
    if (rows > static_cast<int64_t>(UINT32_MAX)) {
      return arrow::Status::Invalid("a Top-N buffer is limited to ", UINT32_MAX, " rows");
    }
    std::vector<uint32_t> order(static_cast<size_t>(rows));
    std::iota(order.begin(), order.end(), 0u);
    const auto before = [&](uint32_t a, uint32_t b) {
      const int c = comparator.Compare(a, b);
      return c != 0 ? c < 0 : seq[a] < seq[b];
    };
    const auto keep = static_cast<size_t>(std::min<int64_t>(node_.n, rows));
    std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(keep), order.end(),
                      before);
    order.resize(keep);

    for (size_t c = 0; c < width; ++c) {
      ARROW_ASSIGN_OR_RAISE(best_[c], Gather(*all[c], order, context_->pool));
    }
    for (size_t k = 0; k < node_.keys.size(); ++k) {
      if (all_keys[k] != nullptr) {
        ARROW_ASSIGN_OR_RAISE(best_keys_[k], Gather(*all_keys[k], order, context_->pool));
      }
    }
    std::vector<int64_t> kept_seq;
    kept_seq.reserve(keep);
    for (const uint32_t i : order) kept_seq.push_back(seq[i]);
    best_seq_ = std::move(kept_seq);
    best_rows_ = static_cast<int64_t>(keep);
    return arrow::Status::OK();
  }

  const PhysicalTopN& node_;
  std::unique_ptr<Operator> input_;
  ExecContext* context_ = nullptr;
  std::unique_ptr<Reservation> reservation_;
  std::unique_ptr<ChargedBytes> charged_;
  bool done_ = false;
  std::vector<std::shared_ptr<arrow::Array>> best_;
  std::vector<std::shared_ptr<arrow::Array>> best_keys_;
  std::vector<int64_t> best_seq_;
  int64_t best_rows_ = 0;
  int64_t next_seq_ = 0;
  int64_t position_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeTopNOperator(const PhysicalTopN& node,
                                           std::unique_ptr<Operator> input) {
  return std::make_unique<TopNOperator>(node, std::move(input));
}

}  // namespace quarry
