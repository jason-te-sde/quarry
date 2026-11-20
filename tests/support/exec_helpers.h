#ifndef QUARRY_TESTS_SUPPORT_EXEC_HELPERS_H_
#define QUARRY_TESTS_SUPPORT_EXEC_HELPERS_H_

// Helpers for tests that run operators: plan a query against a catalog, drain an operator into
// rows of text, build a physical plan by hand.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/catalog.h"
#include "exec/exec_context.h"
#include "exec/operator.h"
#include "plan/physical.h"
#include "planner/physical_planner.h"
#include "planner/rules.h"
#include "support/arrays.h"

namespace quarry::testing {

inline arrow::Result<PhysicalPlan> PlanQuery(const std::string& sql, const Catalog& catalog,
                                             const PlannerOptions& options = {}) {
  ARROW_ASSIGN_OR_RAISE(LogicalPlan bound, ParseAndBind(sql, catalog));
  ARROW_ASSIGN_OR_RAISE(LogicalPlan optimized, Optimize(std::move(bound), options));
  return PlanPhysical(optimized, options);
}

inline const PhysicalScan* FindFirstScan(const PhysicalNode& node) {
  if (node.kind == PhysicalKind::kScan) return static_cast<const PhysicalScan*>(&node);
  for (const PhysicalNode* child : node.children()) {
    if (const PhysicalScan* scan = FindFirstScan(*child)) return scan;
  }
  return nullptr;
}

// An ExecContext with an unlimited budget that lives as long as the object.
struct TestContext {
  MemoryBudget budget{0};
  ExecContext context;
  explicit TestContext(int32_t batch_size = 2048) {
    context.budget = &budget;
    context.batch_size = batch_size;
  }
};

// Opens the operator, pulls every batch and closes it.
inline arrow::Result<std::vector<Batch>> Drain(Operator& op, ExecContext& context) {
  ARROW_RETURN_NOT_OK(op.Open(context));
  std::vector<Batch> out;
  for (;;) {
    auto next = op.Next();
    if (!next.ok()) {
      op.Close();
      return next.status();
    }
    if (!next->has_value()) break;
    out.push_back(std::move(**next));
  }
  op.Close();
  return out;
}

// The live rows of the batches as "cell|cell|..." strings, in order.
inline std::vector<std::string> RowsOf(const std::vector<Batch>& batches) {
  std::vector<std::string> rows;
  for (const Batch& batch : batches) {
    auto compact = batch.Compact(arrow::default_memory_pool());
    EXPECT_TRUE(compact.ok()) << compact.status().ToString();
    if (!compact.ok()) continue;
    for (int64_t i = 0; i < compact->length; ++i) {
      std::string row;
      for (size_t c = 0; c < compact->columns.size(); ++c) {
        row += (c == 0 ? "" : "|") + Cell(*compact->columns[c], i);
      }
      rows.push_back(row);
    }
  }
  return rows;
}

inline int64_t TotalRows(const std::vector<Batch>& batches) {
  int64_t total = 0;
  for (const Batch& b : batches) total += b.active_rows();
  return total;
}

}  // namespace quarry::testing

#endif  // QUARRY_TESTS_SUPPORT_EXEC_HELPERS_H_
