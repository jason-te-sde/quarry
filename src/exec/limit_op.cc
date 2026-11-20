#include <arrow/status.h>

#include <algorithm>
#include <vector>

#include "exec/operators.h"

namespace quarry {
namespace {

class LimitOperator final : public Operator {
 public:
  LimitOperator(const PhysicalLimit& node, std::unique_ptr<Operator> input)
      : remaining_(node.n), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override { return input_->Open(context); }

  arrow::Result<std::optional<Batch>> Next() override {
    if (remaining_ <= 0) return std::optional<Batch>();  // satisfied: the input is not pulled again
    ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, input_->Next());
    if (!batch.has_value()) {
      remaining_ = 0;
      return std::optional<Batch>();
    }
    const int64_t live = batch->active_rows();
    if (live <= remaining_) {
      remaining_ -= live;
      return batch;
    }
    const int64_t keep = remaining_;
    remaining_ = 0;
    if (batch->selection.has_value()) {
      const std::span<const uint32_t> indices = batch->selection->indices();
      std::vector<uint32_t> first(indices.begin(), indices.begin() + keep);
      ARROW_ASSIGN_OR_RAISE(batch->selection,
                            SelectionVector::Make(std::move(first), batch->length));
    } else {
      for (auto& column : batch->columns) column = column->Slice(0, keep);
      batch->length = keep;
    }
    return batch;
  }

  void Close() override { input_->Close(); }

 private:
  int64_t remaining_;
  std::unique_ptr<Operator> input_;
};

}  // namespace

std::unique_ptr<Operator> MakeLimitOperator(const PhysicalLimit& node,
                                            std::unique_ptr<Operator> input) {
  return std::make_unique<LimitOperator>(node, std::move(input));
}

}  // namespace quarry
