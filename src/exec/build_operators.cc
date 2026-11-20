#include "exec/build_operators.h"

#include <arrow/status.h>

#include <algorithm>
#include <chrono>

#include "exec/operators.h"
#include "exec/scan_op.h"

namespace quarry {
namespace {

using Clock = std::chrono::steady_clock;

int64_t NanosSince(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

// Wraps an operator: counts what it returns, times Open, Next and Close, and ends the query when it
// is cancelled or past its deadline.
class ProfiledOperator final : public Operator {
 public:
  ProfiledOperator(std::unique_ptr<Operator> inner, int id, std::string name,
                   std::vector<ProfiledOperator*> inputs)
      : inner_(std::move(inner)), inputs_(std::move(inputs)) {
    stats_.id = id;
    stats_.name = std::move(name);
  }

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    const Clock::time_point start = Clock::now();
    arrow::Status status = inner_->Open(context);
    inclusive_ns_ += NanosSince(start);
    return status;
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (context_->cancelled != nullptr && context_->cancelled->load(std::memory_order_relaxed)) {
      return arrow::Status::Cancelled("query cancelled");
    }
    if (context_->deadline != Clock::time_point::max() && Clock::now() >= context_->deadline) {
      return arrow::Status::Cancelled("query cancelled: the timeout of ", context_->timeout.count(),
                                      " ms was exceeded");
    }
    const Clock::time_point start = Clock::now();
    arrow::Result<std::optional<Batch>> next = inner_->Next();
    inclusive_ns_ += NanosSince(start);
    if (next.ok() && next->has_value()) {
      ++stats_.batches_out;
      stats_.rows_out += (**next).active_rows();
    }
    return next;
  }

  void Close() override {
    const Clock::time_point start = Clock::now();
    inner_->Close();
    inclusive_ns_ += NanosSince(start);
  }

  void CollectProfile(std::vector<OperatorStats>* out) const override {
    OperatorStats stats = stats_;
    int64_t inputs_ns = 0;
    for (const ProfiledOperator* input : inputs_) inputs_ns += input->inclusive_ns_;
    stats.exclusive_ns = std::max<int64_t>(0, inclusive_ns_ - inputs_ns);
    inner_->FillStats(&stats);
    out->push_back(std::move(stats));
    for (const ProfiledOperator* input : inputs_) input->CollectProfile(out);
  }

 private:
  std::unique_ptr<Operator> inner_;
  std::vector<ProfiledOperator*> inputs_;
  OperatorStats stats_;
  int64_t inclusive_ns_ = 0;
  ExecContext* context_ = nullptr;
};

arrow::Result<std::unique_ptr<ProfiledOperator>> Build(const PhysicalNode& node) {
  std::vector<std::unique_ptr<ProfiledOperator>> inputs;
  std::vector<ProfiledOperator*> raw;
  for (const PhysicalNode* child : node.children()) {
    ARROW_ASSIGN_OR_RAISE(auto built, Build(*child));
    raw.push_back(built.get());
    inputs.push_back(std::move(built));
  }
  std::unique_ptr<Operator> inner;
  switch (node.kind) {
    case PhysicalKind::kScan:
      inner = std::make_unique<ScanOperator>(static_cast<const PhysicalScan&>(node));
      break;
    case PhysicalKind::kFilter:
      inner = MakeFilterOperator(static_cast<const PhysicalFilter&>(node), std::move(inputs[0]));
      break;
    case PhysicalKind::kProject:
      inner = MakeProjectOperator(static_cast<const PhysicalProject&>(node), std::move(inputs[0]));
      break;
    case PhysicalKind::kHashJoin:
      // children() lists the probe input first, then the build input.
      inner = MakeHashJoinOperator(static_cast<const PhysicalHashJoin&>(node), std::move(inputs[0]),
                                   std::move(inputs[1]));
      break;
    case PhysicalKind::kHashAggregate:
      inner = MakeHashAggregateOperator(static_cast<const PhysicalHashAggregate&>(node),
                                        std::move(inputs[0]));
      break;
    case PhysicalKind::kSort:
      inner = MakeSortOperator(static_cast<const PhysicalSort&>(node), std::move(inputs[0]));
      break;
    case PhysicalKind::kTopN:
      inner = MakeTopNOperator(static_cast<const PhysicalTopN&>(node), std::move(inputs[0]));
      break;
    case PhysicalKind::kLimit:
      inner = MakeLimitOperator(static_cast<const PhysicalLimit&>(node), std::move(inputs[0]));
      break;
  }
  if (inner == nullptr) return arrow::Status::Invalid("unknown physical node");
  return std::make_unique<ProfiledOperator>(std::move(inner), node.id, PhysicalKindName(node.kind),
                                            std::move(raw));
}

}  // namespace

arrow::Result<std::unique_ptr<Operator>> BuildOperators(const PhysicalNode& root,
                                                        ExecContext& context) {
  if (context.batch_size <= 0) {
    return arrow::Status::Invalid("batch size must be positive, not ", context.batch_size);
  }
  if (context.budget == nullptr) {
    return arrow::Status::Invalid("an ExecContext needs a memory budget (0 = unlimited)");
  }
  ARROW_ASSIGN_OR_RAISE(auto built, Build(root));
  return std::unique_ptr<Operator>(std::move(built));
}

std::vector<OperatorStats> CollectOperatorStats(const Operator& root) {
  std::vector<OperatorStats> stats;
  root.CollectProfile(&stats);
  std::sort(stats.begin(), stats.end(),
            [](const OperatorStats& a, const OperatorStats& b) { return a.id < b.id; });
  return stats;
}

}  // namespace quarry
