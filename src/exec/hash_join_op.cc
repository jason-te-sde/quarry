#include <arrow/status.h>

#include <algorithm>
#include <vector>

#include "exec/join_hash_table.h"
#include "exec/operator_util.h"
#include "exec/operators.h"
#include "expr/eval.h"
#include "vector/gather.h"

namespace quarry {
namespace {

class HashJoinOperator final : public Operator {
 public:
  HashJoinOperator(const PhysicalHashJoin& node, std::unique_ptr<Operator> probe,
                   std::unique_ptr<Operator> build)
      : node_(node), probe_(std::move(probe)), build_(std::move(build)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    reservation_ =
        std::make_unique<Reservation>(*context.budget, ReservationOwner("HashJoin", node_.id));
    charged_ = std::make_unique<ChargedBytes>(reservation_.get());
    ARROW_RETURN_NOT_OK(probe_->Open(context));
    return build_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (!built_) {
      ARROW_RETURN_NOT_OK(Build());
      built_ = true;
    }
    if (table_.rows() == 0) return std::optional<Batch>();  // an empty build side joins nothing
    for (;;) {
      if (!probing_) {
        ARROW_ASSIGN_OR_RAISE(std::optional<Batch> next, probe_->Next());
        if (!next.has_value()) return std::optional<Batch>();
        ARROW_ASSIGN_OR_RAISE(probe_batch_, next->Compact(context_->pool));
        std::vector<std::shared_ptr<arrow::Array>> keys;
        for (const ExprPtr& key : node_.probe_keys) {
          ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*key, probe_batch_, context_->pool));
          keys.push_back(std::move(column));
        }
        ARROW_RETURN_NOT_OK(table_.Lookup(keys, probe_batch_.length, &first_));
        row_ = 0;
        match_ = probe_batch_.length > 0 ? first_[0] : JoinHashTable::kNoRow;
        probing_ = true;
      }
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> out, ProduceBatch());
      if (out.has_value()) return out;
    }
  }

  void Close() override {
    probe_->Close();
    build_->Close();
  }

  void FillStats(OperatorStats* stats) const override {
    if (reservation_ != nullptr) stats->peak_state_bytes = reservation_->peak();
  }

 private:
  arrow::Status Build() {
    const size_t width = node_.build->output_types.size();
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> parts(width);
    int64_t payload_bytes = 0;
    for (;;) {
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> next, build_->Next());
      if (!next.has_value()) break;
      ARROW_ASSIGN_OR_RAISE(Batch batch, next->Compact(context_->pool));
      std::vector<std::shared_ptr<arrow::Array>> keys;
      for (const ExprPtr& key : node_.build_keys) {
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*key, batch, context_->pool));
        keys.push_back(std::move(column));
      }
      ARROW_RETURN_NOT_OK(table_.Insert(keys, batch.length));
      for (size_t c = 0; c < width; ++c) parts[c].push_back(batch.columns[c]);
      payload_bytes += BatchBytes(batch);
      ARROW_RETURN_NOT_OK(charged_->Set(payload_bytes + table_.memory_bytes()));
    }
    build_columns_.clear();
    for (size_t c = 0; c < width; ++c) {
      ARROW_ASSIGN_OR_RAISE(
          auto column,
          ConcatenateArrays(parts[c], node_.build->output_types[c].ToArrow(), context_->pool));
      build_columns_.push_back(std::move(column));
    }
    return arrow::Status::OK();
  }

  // Up to batch_size matches of the current probe batch, from where the last call stopped.
  arrow::Result<std::optional<Batch>> ProduceBatch() {
    std::vector<uint32_t> probe_rows;
    std::vector<uint32_t> build_rows;
    const auto limit = static_cast<size_t>(context_->batch_size);
    while (row_ < probe_batch_.length && probe_rows.size() < limit) {
      if (match_ == JoinHashTable::kNoRow) {
        ++row_;
        match_ =
            row_ < probe_batch_.length ? first_[static_cast<size_t>(row_)] : JoinHashTable::kNoRow;
        continue;
      }
      probe_rows.push_back(static_cast<uint32_t>(row_));
      build_rows.push_back(match_);
      match_ = table_.Next(match_);
    }
    if (row_ >= probe_batch_.length) probing_ = false;
    if (probe_rows.empty()) return std::optional<Batch>();

    const auto n = static_cast<int64_t>(probe_rows.size());
    const auto gather_probe = [&](int index) {
      return Gather(*probe_batch_.columns[static_cast<size_t>(index)], probe_rows, context_->pool);
    };
    const auto gather_build = [&](int index) {
      return Gather(*build_columns_[static_cast<size_t>(index)], build_rows, context_->pool);
    };

    if (node_.residual == nullptr) {
      std::vector<std::shared_ptr<arrow::Array>> columns;
      for (const JoinOutputColumn& out : node_.output_columns) {
        ARROW_ASSIGN_OR_RAISE(auto column,
                              out.from_build ? gather_build(out.index) : gather_probe(out.index));
        columns.push_back(std::move(column));
      }
      return std::optional<Batch>(MakeBatch(std::move(columns), n));
    }

    // The residual sees every probe column and then every build column of the matched pairs.
    std::vector<std::shared_ptr<arrow::Array>> probe_side;
    std::vector<std::shared_ptr<arrow::Array>> build_side;
    std::vector<std::shared_ptr<arrow::Array>> both;
    for (size_t c = 0; c < probe_batch_.columns.size(); ++c) {
      ARROW_ASSIGN_OR_RAISE(auto column, gather_probe(static_cast<int>(c)));
      probe_side.push_back(column);
      both.push_back(std::move(column));
    }
    for (size_t c = 0; c < build_columns_.size(); ++c) {
      ARROW_ASSIGN_OR_RAISE(auto column, gather_build(static_cast<int>(c)));
      build_side.push_back(column);
      both.push_back(std::move(column));
    }
    Batch candidates = MakeBatch(std::move(both), n);
    ARROW_ASSIGN_OR_RAISE(SelectionVector kept,
                          EvaluatePredicate(*node_.residual, candidates, context_->pool));
    if (kept.size() == 0) return std::optional<Batch>();
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const JoinOutputColumn& out : node_.output_columns) {
      columns.push_back(out.from_build ? build_side[static_cast<size_t>(out.index)]
                                       : probe_side[static_cast<size_t>(out.index)]);
    }
    Batch result = MakeBatch(std::move(columns), n);
    if (kept.size() < n) result.selection = std::move(kept);
    return std::optional<Batch>(std::move(result));
  }

  const PhysicalHashJoin& node_;
  std::unique_ptr<Operator> probe_;
  std::unique_ptr<Operator> build_;
  ExecContext* context_ = nullptr;
  std::unique_ptr<Reservation> reservation_;
  std::unique_ptr<ChargedBytes> charged_;
  JoinHashTable table_;
  std::vector<std::shared_ptr<arrow::Array>> build_columns_;
  bool built_ = false;

  // The probe batch being matched: its first match per row, and where the walk stopped.
  bool probing_ = false;
  Batch probe_batch_;
  std::vector<uint32_t> first_;
  int64_t row_ = 0;
  uint32_t match_ = JoinHashTable::kNoRow;
};

}  // namespace

std::unique_ptr<Operator> MakeHashJoinOperator(const PhysicalHashJoin& node,
                                               std::unique_ptr<Operator> probe,
                                               std::unique_ptr<Operator> build) {
  return std::make_unique<HashJoinOperator>(node, std::move(probe), std::move(build));
}

}  // namespace quarry
