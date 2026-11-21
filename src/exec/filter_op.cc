#include <arrow/status.h>

#include "exec/operators.h"
#include "expr/eval.h"

namespace quarry {
namespace {

class FilterOperator final : public Operator {
 public:
  FilterOperator(const PhysicalFilter& node, std::unique_ptr<Operator> input)
      : node_(node), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    return input_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    for (;;) {
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, input_->Next());
      if (!batch.has_value()) return std::optional<Batch>();
      ARROW_ASSIGN_OR_RAISE(SelectionVector selection,
                            EvaluatePredicate(*node_.predicate, *batch, context_->pool));
      if (selection.size() == 0) continue;
      batch->selection = std::move(selection);
      return batch;
    }
  }

  void Close() override { input_->Close(); }

 private:
  const PhysicalFilter& node_;
  std::unique_ptr<Operator> input_;
  ExecContext* context_ = nullptr;
};

}  // namespace

std::unique_ptr<Operator> MakeFilterOperator(const PhysicalFilter& node,
                                             std::unique_ptr<Operator> input) {
  return std::make_unique<FilterOperator>(node, std::move(input));
}

}  // namespace quarry
