#ifndef QUARRY_PLANNER_PLAN_UTIL_H_
#define QUARRY_PLANNER_PLAN_UTIL_H_

#include <arrow/result.h>

#include <functional>
#include <vector>

#include "expr/bound_expr.h"
#include "plan/logical.h"

namespace quarry {

// Old output column index -> new output column index of a node a rule rewrote; -1 for a column
// that no longer exists. A rule that changes the column layout of a node returns the map, and the
// parent redirects its own expressions through it (RewriteChildren).
using ColumnMap = std::vector<int>;

struct Rewritten {
  LogicalPlan plan;
  ColumnMap map;
};

// Redirects every column reference in the tree through `map`.
void RemapColumns(BoundExpr* expr, const ColumnMap& map);
// Adds the indices of the columns the tree references to `out`.
void CollectColumns(const BoundExpr& expr, std::vector<int>* out);
// Splits a tree of ANDs into its conjuncts, in order.
void FlattenAnd(ExprPtr expr, std::vector<ExprPtr>* out);
// The conjuncts joined by AND, left to right; null for none.
ExprPtr AndAll(std::vector<ExprPtr> conjuncts);

// The children of a node as mutable slots, in order.
std::vector<LogicalPlan*> ChildSlots(LogicalNode& node);
std::vector<const LogicalNode*> ChildrenOf(const LogicalNode& node);

// Applies `fn` to each child of `node`, puts the rewritten children back, redirects the node's own
// expressions through the children's maps, and returns the node with the map of its own output
// (identity for a node whose output does not depend on the child's layout: Project, Aggregate).
arrow::Result<Rewritten> RewriteChildren(
    LogicalPlan node, const std::function<arrow::Result<Rewritten>(LogicalPlan)>& fn);

// Redirects the expressions of `node` (not of its children) through the children's maps.
void RemapOwnExpressions(LogicalNode& node, const std::vector<ColumnMap>& child_maps,
                         const std::vector<size_t>& old_child_widths);

}  // namespace quarry

#endif  // QUARRY_PLANNER_PLAN_UTIL_H_
