#include "quarry/engine.h"

#include <arrow/chunked_array.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>

#include "binder/binder.h"
#include "binder/catalog.h"
#include "exec/build_operators.h"
#include "exec/memory_budget.h"
#include "plan/explain.h"
#include "planner/physical_planner.h"
#include "planner/rules.h"

namespace quarry {
namespace {

// The names of the result columns: the select list's, found at the top of the plan.
std::vector<std::string> OutputNames(const PhysicalNode& node) {
  switch (node.kind) {
    case PhysicalKind::kProject:
      return static_cast<const PhysicalProject&>(node).names;
    case PhysicalKind::kFilter:
    case PhysicalKind::kSort:
    case PhysicalKind::kTopN:
    case PhysicalKind::kLimit:
      return OutputNames(*node.children()[0]);
    default: {
      std::vector<std::string> names;
      for (size_t i = 0; i < node.output_types.size(); ++i) {
        names.push_back("column" + std::to_string(i));
      }
      return names;
    }
  }
}

std::string FormatBytes(int64_t bytes) {
  char buffer[32];
  if (bytes < 1024) {
    std::snprintf(buffer, sizeof(buffer), "%lldB", static_cast<long long>(bytes));
  } else if (bytes < int64_t{1024} * 1024) {
    std::snprintf(buffer, sizeof(buffer), "%.1fKiB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1fMiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  }
  return buffer;
}

struct Executed {
  QueryResult result;
  PhysicalPlan plan;
};

}  // namespace

struct Engine::Impl {
  explicit Impl(EngineOptions o) : options(std::move(o)) {}

  arrow::Result<PhysicalPlan> Plan(std::string_view sql) const {
    PlannerOptions planner;
    planner.disabled_rules = options.disabled_rules;
    ARROW_RETURN_NOT_OK(ValidateRuleNames(planner.disabled_rules));
    ARROW_ASSIGN_OR_RAISE(LogicalPlan bound, ParseAndBind(sql, catalog));
    ARROW_ASSIGN_OR_RAISE(LogicalPlan optimized, Optimize(std::move(bound), planner));
    return PlanPhysical(optimized, planner);
  }

  arrow::Result<Executed> Run(std::string_view sql) {
    if (options.batch_size <= 0) {
      return arrow::Status::Invalid("batch size must be positive, not ", options.batch_size);
    }
    if (options.memory_limit_bytes < 0) {
      return arrow::Status::Invalid("the memory limit must not be negative");
    }
    ARROW_ASSIGN_OR_RAISE(PhysicalPlan plan, Plan(sql));

    cancelled.store(false);
    MemoryBudget budget(options.memory_limit_bytes);
    ExecContext context;
    context.budget = &budget;
    context.batch_size = options.batch_size;
    context.cancelled = &cancelled;
    if (options.timeout.count() > 0) {
      context.timeout = options.timeout;
      context.deadline = std::chrono::steady_clock::now() + options.timeout;
    }
    ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> root, BuildOperators(*plan, context));

    ARROW_RETURN_NOT_OK(root->Open(context));
    std::vector<std::vector<std::shared_ptr<arrow::Array>>> chunks(plan->output_types.size());
    arrow::Status status;
    for (;;) {
      auto next = root->Next();
      if (!next.ok()) {
        status = next.status();
        break;
      }
      const std::optional<Batch> batch = std::move(*next);
      if (!batch.has_value()) break;
      auto compact = batch->Compact(context.pool);
      if (!compact.ok()) {
        status = compact.status();
        break;
      }
      for (size_t c = 0; c < chunks.size(); ++c) chunks[c].push_back(compact->columns[c]);
    }
    root->Close();
    ARROW_RETURN_NOT_OK(status);

    const std::vector<std::string> names = OutputNames(*plan);
    std::vector<std::shared_ptr<arrow::Field>> fields;
    std::vector<std::shared_ptr<arrow::ChunkedArray>> columns;
    for (size_t c = 0; c < chunks.size(); ++c) {
      auto type = plan->output_types[c].ToArrow();
      fields.push_back(arrow::field(names[c], type));
      columns.push_back(std::make_shared<arrow::ChunkedArray>(std::move(chunks[c]), type));
    }
    Executed out;
    out.result.table = arrow::Table::Make(arrow::schema(fields), std::move(columns));
    for (const OperatorStats& s : CollectOperatorStats(*root)) {
      OperatorProfile profile;
      profile.id = s.id;
      profile.name = s.name;
      profile.rows_out = s.rows_out;
      profile.batches_out = s.batches_out;
      profile.exclusive_ns = s.exclusive_ns;
      profile.peak_state_bytes = s.peak_state_bytes;
      profile.spilled_bytes = s.spilled_bytes;
      profile.scan_buffer_bytes = s.scan_buffer_bytes;
      out.result.profile.push_back(std::move(profile));
    }
    out.plan = std::move(plan);
    return out;
  }

  EngineOptions options;
  Catalog catalog;
  std::atomic<bool> busy{false};
  std::atomic<bool> cancelled{false};
};

namespace {

// Marks the engine busy for the life of the object, or says it already is.
class BusyGuard {
 public:
  explicit BusyGuard(std::atomic<bool>& flag) : flag_(flag), owns_(!flag.exchange(true)) {}
  ~BusyGuard() {
    if (owns_) flag_.store(false);
  }
  BusyGuard(const BusyGuard&) = delete;
  BusyGuard& operator=(const BusyGuard&) = delete;
  bool owns() const { return owns_; }

 private:
  std::atomic<bool>& flag_;
  bool owns_;
};

arrow::Status Busy() {
  return arrow::Status::Invalid(
      "engine is busy: another query is running on this Engine; use one Engine per thread, or wait "
      "for it to finish");
}

}  // namespace

Engine::Engine(EngineOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

Engine::~Engine() = default;

arrow::Status Engine::RegisterTable(std::string name, std::vector<std::filesystem::path> files) {
  BusyGuard guard(impl_->busy);
  if (!guard.owns()) return Busy();
  return impl_->catalog.AddTable(std::move(name), std::move(files));
}

arrow::Status Engine::RegisterDirectory(const std::filesystem::path& dir) {
  BusyGuard guard(impl_->busy);
  if (!guard.owns()) return Busy();
  return impl_->catalog.AddDirectory(dir);
}

arrow::Result<QueryResult> Engine::Execute(std::string_view sql) {
  BusyGuard guard(impl_->busy);
  if (!guard.owns()) return Busy();
  ARROW_ASSIGN_OR_RAISE(Executed executed, impl_->Run(sql));
  return std::move(executed.result);
}

arrow::Result<std::string> Engine::Explain(std::string_view sql, bool analyze) {
  BusyGuard guard(impl_->busy);
  if (!guard.owns()) return Busy();
  if (!analyze) {
    ARROW_ASSIGN_OR_RAISE(PhysicalPlan plan, impl_->Plan(sql));
    return ::quarry::Explain(*plan);
  }
  ARROW_ASSIGN_OR_RAISE(Executed executed, impl_->Run(sql));
  // Line i of the plan text is the node with id i (pre-order, a join's probe side first).
  std::istringstream lines(::quarry::Explain(*executed.plan));
  std::string out;
  std::string line;
  size_t index = 0;
  while (std::getline(lines, line)) {
    out += line;
    if (index < executed.result.profile.size()) {
      const OperatorProfile& p = executed.result.profile[index];
      char time[32];
      std::snprintf(time, sizeof(time), "%.3f", static_cast<double>(p.exclusive_ns) / 1e6);
      out += " rows=" + std::to_string(p.rows_out) + " batches=" + std::to_string(p.batches_out) +
             " time=" + time + "ms peak=" + FormatBytes(p.peak_state_bytes);
      if (p.scan_buffer_bytes > 0) out += " buffer=" + FormatBytes(p.scan_buffer_bytes);
      if (p.spilled_bytes > 0) out += " spilled=" + FormatBytes(p.spilled_bytes);
    }
    out += '\n';
    ++index;
  }
  return out;
}

void Engine::Cancel() { impl_->cancelled.store(true); }

}  // namespace quarry
