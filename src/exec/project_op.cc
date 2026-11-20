#include <arrow/status.h>

#include "exec/operator_util.h"
#include "exec/operators.h"
#include "expr/eval.h"

namespace quarry {
namespace {

class ProjectOperator final : public Operator {
 public:
  ProjectOperator(const PhysicalProject& node, std::unique_ptr<Operator> input)
      : node_(node), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    return input_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, input_->Next());
    if (!batch.has_value()) return std::optional<Batch>();
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(node_.exprs.size());
    for (const ExprPtr& expr : node_.exprs) {
      ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*expr, *batch, context_->pool));
      columns.push_back(std::move(column));
    }
    return std::optional<Batch>(MakeBatch(std::move(columns), batch->active_rows()));
  }

  void Close() override { input_->Close(); }

 private:
  const PhysicalProject& node_;
  std::unique_ptr<Operator> input_;
  ExecContext* context_ = nullptr;
};

}  // namespace

std::unique_ptr<Operator> MakeProjectOperator(const PhysicalProject& node,
                                              std::unique_ptr<Operator> input) {
  return std::make_unique<ProjectOperator>(node, std::move(input));
}

}  // namespace quarry
