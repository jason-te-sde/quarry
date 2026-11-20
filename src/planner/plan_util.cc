#include "planner/plan_util.h"

#include <utility>

namespace quarry {

void RemapColumns(BoundExpr* expr, const ColumnMap& map) {
  if (expr->kind == ExprKind::kColumn) {
    expr->column_index = map[static_cast<size_t>(expr->column_index)];
  }
  for (ExprPtr& child : expr->children) RemapColumns(child.get(), map);
}

void CollectColumns(const BoundExpr& expr, std::vector<int>* out) {
  if (expr.kind == ExprKind::kColumn) out->push_back(expr.column_index);
  for (const ExprPtr& child : expr.children) CollectColumns(*child, out);
}

void FlattenAnd(ExprPtr expr, std::vector<ExprPtr>* out) {
  if (expr->kind == ExprKind::kBinary && expr->op == BinaryOp::kAnd) {
    FlattenAnd(std::move(expr->children[0]), out);
    FlattenAnd(std::move(expr->children[1]), out);
    return;
  }
  out->push_back(std::move(expr));
}

ExprPtr AndAll(std::vector<ExprPtr> conjuncts) {
  ExprPtr result;
  for (ExprPtr& c : conjuncts) {
    if (result == nullptr) {
      result = std::move(c);
      continue;
    }
    // Both operands are BOOLEAN, so MakeBinary cannot fail.
    result = MakeBinary(BinaryOp::kAnd, std::move(result), std::move(c)).ValueOrDie();
  }
  return result;
}

std::vector<LogicalPlan*> ChildSlots(LogicalNode& node) {
  switch (node.kind) {
    case LogicalKind::kScan:
      return {};
    case LogicalKind::kFilter:
      return {&static_cast<LogicalFilter&>(node).child};
    case LogicalKind::kProject:
      return {&static_cast<LogicalProject&>(node).child};
    case LogicalKind::kAggregate:
      return {&static_cast<LogicalAggregate&>(node).child};
    case LogicalKind::kSort:
      return {&static_cast<LogicalSort&>(node).child};
    case LogicalKind::kLimit:
      return {&static_cast<LogicalLimit&>(node).child};
    case LogicalKind::kJoin: {
      auto& join = static_cast<LogicalJoin&>(node);
      return {&join.left, &join.right};
    }
    case LogicalKind::kCrossProduct: {
      std::vector<LogicalPlan*> out;
      out.reserve(static_cast<LogicalCrossProduct&>(node).children.size());
      for (LogicalPlan& c : static_cast<LogicalCrossProduct&>(node).children) out.push_back(&c);
      return out;
    }
  }
  return {};
}

std::vector<const LogicalNode*> ChildrenOf(const LogicalNode& node) {
  std::vector<const LogicalNode*> out;
  for (const LogicalPlan* slot : ChildSlots(const_cast<LogicalNode&>(node))) {
    out.push_back(slot->get());
  }
  return out;
}

void RemapOwnExpressions(LogicalNode& node, const std::vector<ColumnMap>& child_maps,
                         const std::vector<size_t>& old_child_widths) {
  switch (node.kind) {
    case LogicalKind::kFilter:
      RemapColumns(static_cast<LogicalFilter&>(node).predicate.get(), child_maps[0]);
      break;
    case LogicalKind::kProject:
      for (ExprPtr& e : static_cast<LogicalProject&>(node).exprs) {
        RemapColumns(e.get(), child_maps[0]);
      }
      break;
    case LogicalKind::kAggregate: {
      auto& aggregate = static_cast<LogicalAggregate&>(node);
      for (ExprPtr& e : aggregate.groups) RemapColumns(e.get(), child_maps[0]);
      for (AggregateExpr& a : aggregate.aggregates) {
        if (a.arg != nullptr) RemapColumns(a.arg.get(), child_maps[0]);
      }
      break;
    }
    case LogicalKind::kSort:
      for (SortKey& k : static_cast<LogicalSort&>(node).keys) {
        RemapColumns(k.expr.get(), child_maps[0]);
      }
      break;
    case LogicalKind::kJoin: {
      auto& join = static_cast<LogicalJoin&>(node);
      for (ExprPtr& k : join.left_keys) RemapColumns(k.get(), child_maps[0]);
      for (ExprPtr& k : join.right_keys) RemapColumns(k.get(), child_maps[1]);
      if (join.residual != nullptr) {
        // The residual indexes left output followed by right output.
        const size_t new_left_width = join.left->output_types().size();
        ColumnMap combined(old_child_widths[0] + old_child_widths[1], -1);
        for (size_t i = 0; i < old_child_widths[0]; ++i) combined[i] = child_maps[0][i];
        for (size_t i = 0; i < old_child_widths[1]; ++i) {
          const int m = child_maps[1][i];
          combined[old_child_widths[0] + i] = m < 0 ? -1 : static_cast<int>(new_left_width) + m;
        }
        RemapColumns(join.residual.get(), combined);
      }
      break;
    }
    case LogicalKind::kScan:
    case LogicalKind::kLimit:
    case LogicalKind::kCrossProduct:
      break;
  }
}

arrow::Result<Rewritten> RewriteChildren(
    LogicalPlan node, const std::function<arrow::Result<Rewritten>(LogicalPlan)>& fn) {
  const std::vector<LogicalPlan*> slots = ChildSlots(*node);
  std::vector<ColumnMap> maps;
  std::vector<size_t> old_widths;
  std::vector<size_t> new_widths;
  for (LogicalPlan* slot : slots) {
    old_widths.push_back((*slot)->output_types().size());
    ARROW_ASSIGN_OR_RAISE(Rewritten child, fn(std::move(*slot)));
    *slot = std::move(child.plan);
    new_widths.push_back((*slot)->output_types().size());
    maps.push_back(std::move(child.map));
  }
  RemapOwnExpressions(*node, maps, old_widths);

  ColumnMap map;
  switch (node->kind) {
    case LogicalKind::kFilter:
    case LogicalKind::kSort:
    case LogicalKind::kLimit:
      map = std::move(maps[0]);
      break;
    case LogicalKind::kJoin:
    case LogicalKind::kCrossProduct: {
      int offset = 0;
      for (size_t c = 0; c < maps.size(); ++c) {
        for (const int m : maps[c]) map.push_back(m < 0 ? -1 : offset + m);
        offset += static_cast<int>(new_widths[c]);
      }
      break;
    }
    case LogicalKind::kScan:
    case LogicalKind::kProject:
    case LogicalKind::kAggregate: {
      const size_t width = node->output_types().size();
      for (size_t i = 0; i < width; ++i) map.push_back(static_cast<int>(i));
      break;
    }
  }
  return Rewritten{std::move(node), std::move(map)};
}

}  // namespace quarry
