#ifndef QUARRY_PLANNER_RULES_H_
#define QUARRY_PLANNER_RULES_H_

#include <arrow/result.h>
#include <arrow/status.h>

#include <array>
#include <set>
#include <string>
#include <string_view>

#include "plan/logical.h"

namespace quarry {

// The six rules that can be switched off (the CLI's --disable-rule). Optimize applies them in the
// order fold_constants, push_filters, order_joins, pick_build_side, prune_columns; PlanPhysical
// reads prune_row_groups. Join construction is not among them: without it a comma join has no plan.
inline constexpr std::array<std::string_view, 6> kRuleNames = {
    "fold_constants", "push_filters", "prune_row_groups",
    "prune_columns",  "order_joins",  "pick_build_side"};

struct PlannerOptions {
  std::set<std::string> disabled_rules;
};

bool IsRuleEnabled(const PlannerOptions& options, std::string_view rule);

// Invalid, naming the unknown rule and listing the six valid names, when a name is not a rule.
arrow::Status ValidateRuleNames(const std::set<std::string>& names);

// Rewrites a bound plan: fold_constants, push_filters, join construction (always on), order_joins,
// pick_build_side, prune_columns, in that order, each skipped when disabled. The result produces
// the same rows as the input, in the same column order. A comma join whose relations are not
// connected by equality predicates is Invalid: "planner error at <line>:<column>: requires a cross
// product between <a> and <b>", the position being where <b>, the relation that cannot be reached,
// is written in the SQL text. See docs/design/0005-optimizer.md.
arrow::Result<LogicalPlan> Optimize(LogicalPlan plan, const PlannerOptions& options);

}  // namespace quarry

#endif  // QUARRY_PLANNER_RULES_H_
