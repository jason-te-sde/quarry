#include "planner/rules.h"

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/status.h>

#include <algorithm>
#include <functional>
#include <numeric>
#include <utility>

#include "expr/eval.h"
#include "planner/cardinality.h"
#include "planner/join_graph.h"
#include "planner/plan_util.h"
#include "vector/batch.h"

namespace quarry {
namespace {

// ---- fold_constants ----

// Replaces a subtree that uses no column by the literal it evaluates to. The evaluator does the
// computing, so the literal has exactly the type the binder gave the subtree. A subtree that fails
// to evaluate (an overflow, say) is left as it is: the error belongs to the query's run, and rows
// that never reach it must not fail.
void FoldExpr(ExprPtr* slot) {
  BoundExpr& e = **slot;
  for (ExprPtr& child : e.children) FoldExpr(&child);
  if (e.kind == ExprKind::kLiteral || e.kind == ExprKind::kColumn) return;
  for (const ExprPtr& child : e.children) {
    if (child->kind != ExprKind::kLiteral) return;
  }
  Batch one_row;
  one_row.length = 1;
  auto array = Evaluate(e, one_row, arrow::default_memory_pool());
  if (!array.ok() || (*array)->length() != 1) return;
  auto scalar = (*array)->GetScalar(0);
  if (!scalar.ok()) return;
  auto literal = MakeLiteral(*scalar);
  if (!literal.ok() || (*literal)->type != e.type) return;
  *slot = std::move(*literal);
}

void FoldNode(LogicalNode& node) {
  switch (node.kind) {
    case LogicalKind::kScan:
      for (ExprPtr& f : static_cast<LogicalScan&>(node).filters) FoldExpr(&f);
      break;
    case LogicalKind::kFilter:
      FoldExpr(&static_cast<LogicalFilter&>(node).predicate);
      break;
    case LogicalKind::kProject:
      for (ExprPtr& e : static_cast<LogicalProject&>(node).exprs) FoldExpr(&e);
      break;
    case LogicalKind::kAggregate: {
      auto& aggregate = static_cast<LogicalAggregate&>(node);
      for (ExprPtr& e : aggregate.groups) FoldExpr(&e);
      for (AggregateExpr& a : aggregate.aggregates) {
        if (a.arg != nullptr) FoldExpr(&a.arg);
      }
      break;
    }
    case LogicalKind::kSort:
      for (SortKey& k : static_cast<LogicalSort&>(node).keys) FoldExpr(&k.expr);
      break;
    case LogicalKind::kJoin: {
      auto& join = static_cast<LogicalJoin&>(node);
      for (ExprPtr& k : join.left_keys) FoldExpr(&k);
      for (ExprPtr& k : join.right_keys) FoldExpr(&k);
      if (join.residual != nullptr) FoldExpr(&join.residual);
      break;
    }
    case LogicalKind::kLimit:
    case LogicalKind::kCrossProduct:
      break;
  }
  for (LogicalPlan* child : ChildSlots(node)) FoldNode(**child);
}

// ---- push_filters ----

void ColumnRange(const BoundExpr& e, int* lo, int* hi) {
  std::vector<int> columns;
  CollectColumns(e, &columns);
  *lo = columns.empty() ? -1 : *std::min_element(columns.begin(), columns.end());
  *hi = columns.empty() ? -1 : *std::max_element(columns.begin(), columns.end());
}

void ShiftColumns(BoundExpr* e, int delta) {
  if (e->kind == ExprKind::kColumn) e->column_index += delta;
  for (ExprPtr& child : e->children) ShiftColumns(child.get(), delta);
}

// Copies `conjunct` (which indexes the output of `target`) into the part of `target` that
// produces all of its columns: a scan's filters, one side of an inner join, one child of a cross
// product. False when it uses columns of two inputs or the target takes no filters.
bool PushInto(LogicalNode& target, const BoundExpr& conjunct) {
  int lo = 0;
  int hi = 0;
  ColumnRange(conjunct, &lo, &hi);
  switch (target.kind) {
    case LogicalKind::kScan:
      static_cast<LogicalScan&>(target).filters.push_back(conjunct.Clone());
      return true;
    case LogicalKind::kJoin: {
      auto& join = static_cast<LogicalJoin&>(target);
      const int left_width = static_cast<int>(join.left->output_types().size());
      if (hi < left_width) return PushInto(*join.left, conjunct);
      if (lo < left_width) return false;
      ExprPtr shifted = conjunct.Clone();
      ShiftColumns(shifted.get(), -left_width);
      return PushInto(*join.right, *shifted);
    }
    case LogicalKind::kCrossProduct: {
      auto& cross = static_cast<LogicalCrossProduct&>(target);
      int offset = 0;
      for (size_t i = 0; i < cross.children.size(); ++i) {
        const int width = static_cast<int>(cross.children[i]->output_types().size());
        if (lo < 0 || (lo >= offset && hi < offset + width)) {
          ExprPtr shifted = conjunct.Clone();
          ShiftColumns(shifted.get(), -offset);
          return PushInto(*cross.children[i], *shifted);
        }
        offset += width;
      }
      return false;
    }
    default:
      return false;
  }
}

LogicalPlan PushFilters(LogicalPlan node) {
  for (LogicalPlan* slot : ChildSlots(*node)) *slot = PushFilters(std::move(*slot));
  if (node->kind != LogicalKind::kFilter) return node;
  auto& filter = static_cast<LogicalFilter&>(*node);
  std::vector<ExprPtr> conjuncts;
  FlattenAnd(std::move(filter.predicate), &conjuncts);
  std::vector<ExprPtr> kept;
  for (ExprPtr& c : conjuncts) {
    if (!PushInto(*filter.child, *c)) kept.push_back(std::move(c));
  }
  if (kept.empty()) return std::move(filter.child);
  filter.predicate = AndAll(std::move(kept));
  return node;
}

// ---- join construction, order_joins ----

arrow::Result<Rewritten> BuildJoins(LogicalPlan node, bool greedy) {
  const bool filter_over_cross =
      node->kind == LogicalKind::kFilter &&
      static_cast<LogicalFilter&>(*node).child->kind == LogicalKind::kCrossProduct;
  if (filter_over_cross || node->kind == LogicalKind::kCrossProduct) {
    std::vector<ExprPtr> conjuncts;
    LogicalPlan cross;
    if (filter_over_cross) {
      auto& filter = static_cast<LogicalFilter&>(*node);
      FlattenAnd(std::move(filter.predicate), &conjuncts);
      cross = std::move(filter.child);
    } else {
      cross = std::move(node);
    }
    auto& children = static_cast<LogicalCrossProduct&>(*cross).children;
    ARROW_ASSIGN_OR_RAISE(JoinTree tree,
                          BuildJoinTree(std::move(children), std::move(conjuncts), greedy));
    if (tree.leftover.empty()) return Rewritten{std::move(tree.plan), std::move(tree.map)};
    auto filter = std::make_unique<LogicalFilter>();
    filter->child = std::move(tree.plan);
    filter->predicate = AndAll(std::move(tree.leftover));
    return Rewritten{std::move(filter), std::move(tree.map)};
  }
  return RewriteChildren(std::move(node), [greedy](LogicalPlan child) {
    return BuildJoins(std::move(child), greedy);
  });
}

// ---- pick_build_side ----

// Makes the smaller input the right one, which is the build side. A tie keeps the order as written.
arrow::Result<Rewritten> PickBuildSide(LogicalPlan node) {
  ARROW_ASSIGN_OR_RAISE(Rewritten rewritten, RewriteChildren(std::move(node), PickBuildSide));
  if (rewritten.plan->kind != LogicalKind::kJoin) return rewritten;
  auto& join = static_cast<LogicalJoin&>(*rewritten.plan);
  if (!(EstimateRows(*join.left) < EstimateRows(*join.right))) return rewritten;

  const int left_width = static_cast<int>(join.left->output_types().size());
  const int right_width = static_cast<int>(join.right->output_types().size());
  ColumnMap swap(static_cast<size_t>(left_width + right_width));
  for (int i = 0; i < left_width + right_width; ++i) {
    swap[static_cast<size_t>(i)] = i < left_width ? i + right_width : i - left_width;
  }
  std::swap(join.left, join.right);
  std::swap(join.left_keys, join.right_keys);
  if (join.residual != nullptr) RemapColumns(join.residual.get(), swap);
  for (int& m : rewritten.map) {
    if (m >= 0) m = swap[static_cast<size_t>(m)];
  }
  return rewritten;
}

// ---- prune_columns ----

void Mark(const BoundExpr* e, size_t offset, size_t width, std::vector<bool>* required) {
  if (e == nullptr) return;
  std::vector<int> columns;
  CollectColumns(*e, &columns);
  for (const int c : columns) {
    const size_t index = static_cast<size_t>(c);
    if (index >= offset && index < offset + width) (*required)[index - offset] = true;
  }
}

ColumnMap IdentityMap(size_t width) {
  ColumnMap map(width);
  std::iota(map.begin(), map.end(), 0);
  return map;
}

// Rewrites `node` so that it produces at least the columns marked in `required` (a mask over its
// current output) and nothing it does not need itself; the map says where each one went.
arrow::Result<Rewritten> Prune(LogicalPlan node, std::vector<bool> required) {
  switch (node->kind) {
    case LogicalKind::kScan: {
      auto& scan = static_cast<LogicalScan&>(*node);
      std::vector<bool> keep = required;
      for (const ExprPtr& f : scan.filters) Mark(f.get(), 0, keep.size(), &keep);
      // A scan must produce something, so that the rows can be counted: keep the first column.
      if (!keep.empty() && std::none_of(keep.begin(), keep.end(), [](bool b) { return b; })) {
        keep[0] = true;
      }
      ColumnMap map(keep.size(), -1);
      std::vector<int> columns;
      for (size_t i = 0; i < keep.size(); ++i) {
        if (!keep[i]) continue;
        map[i] = static_cast<int>(columns.size());
        columns.push_back(scan.columns[i]);
      }
      scan.columns = std::move(columns);
      for (ExprPtr& f : scan.filters) RemapColumns(f.get(), map);
      return Rewritten{std::move(node), std::move(map)};
    }
    case LogicalKind::kFilter: {
      auto& filter = static_cast<LogicalFilter&>(*node);
      Mark(filter.predicate.get(), 0, required.size(), &required);
      ARROW_ASSIGN_OR_RAISE(Rewritten child, Prune(std::move(filter.child), std::move(required)));
      filter.child = std::move(child.plan);
      RemapColumns(filter.predicate.get(), child.map);
      return Rewritten{std::move(node), std::move(child.map)};
    }
    case LogicalKind::kSort: {
      auto& sort = static_cast<LogicalSort&>(*node);
      for (const SortKey& k : sort.keys) Mark(k.expr.get(), 0, required.size(), &required);
      ARROW_ASSIGN_OR_RAISE(Rewritten child, Prune(std::move(sort.child), std::move(required)));
      sort.child = std::move(child.plan);
      for (SortKey& k : sort.keys) RemapColumns(k.expr.get(), child.map);
      return Rewritten{std::move(node), std::move(child.map)};
    }
    case LogicalKind::kLimit: {
      auto& limit = static_cast<LogicalLimit&>(*node);
      ARROW_ASSIGN_OR_RAISE(Rewritten child, Prune(std::move(limit.child), std::move(required)));
      limit.child = std::move(child.plan);
      return Rewritten{std::move(node), std::move(child.map)};
    }
    case LogicalKind::kProject: {
      auto& project = static_cast<LogicalProject&>(*node);
      const size_t width = project.child->output_types().size();
      std::vector<bool> need(width, false);
      for (const ExprPtr& e : project.exprs) Mark(e.get(), 0, width, &need);
      ARROW_ASSIGN_OR_RAISE(Rewritten child, Prune(std::move(project.child), std::move(need)));
      project.child = std::move(child.plan);
      for (ExprPtr& e : project.exprs) RemapColumns(e.get(), child.map);
      return Rewritten{std::move(node), IdentityMap(project.exprs.size())};
    }
    case LogicalKind::kAggregate: {
      auto& aggregate = static_cast<LogicalAggregate&>(*node);
      const size_t width = aggregate.child->output_types().size();
      std::vector<bool> need(width, false);
      for (const ExprPtr& e : aggregate.groups) Mark(e.get(), 0, width, &need);
      for (const AggregateExpr& a : aggregate.aggregates) Mark(a.arg.get(), 0, width, &need);
      ARROW_ASSIGN_OR_RAISE(Rewritten child, Prune(std::move(aggregate.child), std::move(need)));
      aggregate.child = std::move(child.plan);
      for (ExprPtr& e : aggregate.groups) RemapColumns(e.get(), child.map);
      for (AggregateExpr& a : aggregate.aggregates) {
        if (a.arg != nullptr) RemapColumns(a.arg.get(), child.map);
      }
      return Rewritten{std::move(node),
                       IdentityMap(aggregate.groups.size() + aggregate.aggregates.size())};
    }
    case LogicalKind::kJoin: {
      auto& join = static_cast<LogicalJoin&>(*node);
      const size_t left_width = join.left->output_types().size();
      const size_t right_width = join.right->output_types().size();
      std::vector<bool> need_left(required.begin(),
                                  required.begin() + static_cast<std::ptrdiff_t>(left_width));
      std::vector<bool> need_right(required.begin() + static_cast<std::ptrdiff_t>(left_width),
                                   required.end());
      for (const ExprPtr& k : join.left_keys) Mark(k.get(), 0, left_width, &need_left);
      for (const ExprPtr& k : join.right_keys) Mark(k.get(), 0, right_width, &need_right);
      Mark(join.residual.get(), 0, left_width, &need_left);
      Mark(join.residual.get(), left_width, right_width, &need_right);
      ARROW_ASSIGN_OR_RAISE(Rewritten left, Prune(std::move(join.left), std::move(need_left)));
      ARROW_ASSIGN_OR_RAISE(Rewritten right, Prune(std::move(join.right), std::move(need_right)));
      join.left = std::move(left.plan);
      join.right = std::move(right.plan);
      const size_t new_left_width = join.left->output_types().size();
      for (ExprPtr& k : join.left_keys) RemapColumns(k.get(), left.map);
      for (ExprPtr& k : join.right_keys) RemapColumns(k.get(), right.map);
      ColumnMap map(left_width + right_width, -1);
      for (size_t i = 0; i < left_width; ++i) map[i] = left.map[i];
      for (size_t i = 0; i < right_width; ++i) {
        map[left_width + i] =
            right.map[i] < 0 ? -1 : static_cast<int>(new_left_width) + right.map[i];
      }
      if (join.residual != nullptr) RemapColumns(join.residual.get(), map);
      return Rewritten{std::move(node), std::move(map)};
    }
    case LogicalKind::kCrossProduct: {
      auto& cross = static_cast<LogicalCrossProduct&>(*node);
      ColumnMap map;
      size_t old_offset = 0;
      int new_offset = 0;
      for (LogicalPlan& child : cross.children) {
        const size_t width = child->output_types().size();
        std::vector<bool> need(required.begin() + static_cast<std::ptrdiff_t>(old_offset),
                               required.begin() + static_cast<std::ptrdiff_t>(old_offset + width));
        ARROW_ASSIGN_OR_RAISE(Rewritten pruned, Prune(std::move(child), std::move(need)));
        child = std::move(pruned.plan);
        for (const int m : pruned.map) map.push_back(m < 0 ? -1 : new_offset + m);
        new_offset += static_cast<int>(child->output_types().size());
        old_offset += width;
      }
      return Rewritten{std::move(node), std::move(map)};
    }
  }
  return arrow::Status::Invalid("unknown logical node");
}

}  // namespace

bool IsRuleEnabled(const PlannerOptions& options, std::string_view rule) {
  return options.disabled_rules.count(std::string(rule)) == 0;
}

arrow::Status ValidateRuleNames(const std::set<std::string>& names) {
  for (const std::string& name : names) {
    if (std::find(kRuleNames.begin(), kRuleNames.end(), name) != kRuleNames.end()) continue;
    std::string valid;
    for (const std::string_view rule : kRuleNames) {
      valid += (valid.empty() ? "" : ", ") + std::string(rule);
    }
    return arrow::Status::Invalid("unknown rule '" + name + "': the rules are " + valid);
  }
  return arrow::Status::OK();
}

arrow::Result<LogicalPlan> Optimize(LogicalPlan plan, const PlannerOptions& options) {
  ARROW_RETURN_NOT_OK(ValidateRuleNames(options.disabled_rules));
  if (IsRuleEnabled(options, "fold_constants")) FoldNode(*plan);
  if (IsRuleEnabled(options, "push_filters")) plan = PushFilters(std::move(plan));

  ARROW_ASSIGN_OR_RAISE(Rewritten joined,
                        BuildJoins(std::move(plan), IsRuleEnabled(options, "order_joins")));
  plan = std::move(joined.plan);

  if (IsRuleEnabled(options, "pick_build_side")) {
    ARROW_ASSIGN_OR_RAISE(Rewritten built, PickBuildSide(std::move(plan)));
    plan = std::move(built.plan);
  }
  if (IsRuleEnabled(options, "prune_columns")) {
    std::vector<bool> all(plan->output_types().size(), true);
    ARROW_ASSIGN_OR_RAISE(Rewritten pruned, Prune(std::move(plan), std::move(all)));
    plan = std::move(pruned.plan);
  }
  return plan;
}

}  // namespace quarry
