#include <arrow/status.h>

#include <algorithm>
#include <vector>

#include "exec/agg_hash_table.h"
#include "exec/operator_util.h"
#include "exec/operators.h"
#include "expr/eval.h"

namespace quarry {
namespace {

class HashAggregateOperator final : public Operator {
 public:
  HashAggregateOperator(const PhysicalHashAggregate& node, std::unique_ptr<Operator> input)
      : node_(node), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    reservation_ =
        std::make_unique<Reservation>(*context.budget, ReservationOwner("HashAggregate", node_.id));
    std::vector<LogicalType> key_types;
    key_types.reserve(node_.groups.size());
    for (const ExprPtr& group : node_.groups) key_types.push_back(group->type);
    std::vector<AggSpec> specs;
    specs.reserve(node_.aggregates.size());
    for (const AggregateExpr& a : node_.aggregates) {
      AggSpec spec;
      spec.func = a.func;
      spec.arg_type = a.arg != nullptr ? a.arg->type : LogicalType::Int64();
      spec.result_type = a.result_type;
      specs.push_back(spec);
    }
    table_ =
        std::make_unique<AggHashTable>(std::move(key_types), std::move(specs), reservation_.get());
    return input_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (!consumed_) {
      ARROW_RETURN_NOT_OK(Consume());
      consumed_ = true;
    }
    if (next_group_ >= table_->num_groups()) return std::optional<Batch>();
    const int64_t count =
        std::min<int64_t>(context_->batch_size, table_->num_groups() - next_group_);
    ARROW_ASSIGN_OR_RAISE(auto columns, table_->Finish(next_group_, count, context_->pool));
    next_group_ += count;
    return std::optional<Batch>(MakeBatch(std::move(columns), count));
  }

  void Close() override { input_->Close(); }

  void FillStats(OperatorStats* stats) const override {
    if (reservation_ != nullptr) stats->peak_state_bytes = reservation_->peak();
  }

 private:
  arrow::Status Consume() {
    for (;;) {
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, input_->Next());
      if (!batch.has_value()) break;
      std::vector<std::shared_ptr<arrow::Array>> keys;
      for (const ExprPtr& group : node_.groups) {
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*group, *batch, context_->pool));
        keys.push_back(std::move(column));
      }
      std::vector<std::shared_ptr<arrow::Array>> args;
      for (const AggregateExpr& a : node_.aggregates) {
        if (a.arg == nullptr) {
          args.push_back(nullptr);
          continue;
        }
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*a.arg, *batch, context_->pool));
        args.push_back(std::move(column));
      }
      ARROW_RETURN_NOT_OK(table_->Update(keys, args, batch->active_rows()));
    }
    // Without GROUP BY the answer is one row even when no row came in.
    if (node_.groups.empty()) ARROW_RETURN_NOT_OK(table_->EnsureGlobalGroup());
    return arrow::Status::OK();
  }

  const PhysicalHashAggregate& node_;
  std::unique_ptr<Operator> input_;
  ExecContext* context_ = nullptr;
  std::unique_ptr<Reservation> reservation_;
  std::unique_ptr<AggHashTable> table_;
  bool consumed_ = false;
  int64_t next_group_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeHashAggregateOperator(const PhysicalHashAggregate& node,
                                                    std::unique_ptr<Operator> input) {
  return std::make_unique<HashAggregateOperator>(node, std::move(input));
}

}  // namespace quarry
