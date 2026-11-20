#include "planner/physical_planner.h"

#include <arrow/scalar.h>

#include <algorithm>
#include <optional>
#include <utility>

#include "binder/catalog.h"
#include "planner/cardinality.h"
#include "planner/plan_util.h"

namespace quarry {
namespace {

template <typename T>
std::unique_ptr<T> Make(const LogicalNode& logical) {
  auto node = std::make_unique<T>();
  node->estimated_rows = EstimateRows(logical);
  node->output_types = logical.output_types();
  return node;
}

std::vector<ExprPtr> CloneAll(const std::vector<ExprPtr>& exprs) {
  std::vector<ExprPtr> out;
  out.reserve(exprs.size());
  for (const ExprPtr& e : exprs) out.push_back(e->Clone());
  return out;
}

std::vector<SortKey> CloneKeys(const std::vector<SortKey>& keys) {
  std::vector<SortKey> out;
  out.reserve(keys.size());
  for (const SortKey& k : keys) {
    SortKey copy;
    copy.expr = k.expr->Clone();
    copy.ascending = k.ascending;
    copy.nulls_first = k.nulls_first;
    out.push_back(std::move(copy));
  }
  return out;
}

bool IsRealLiteral(const BoundExpr& e) {
  return e.kind == ExprKind::kLiteral && e.literal != nullptr && e.literal->is_valid;
}

// The comparison with its operands swapped: a < b is b > a.
bool Flip(BinaryOp op, BinaryOp* flipped) {
  switch (op) {
    case BinaryOp::kEq:
      *flipped = BinaryOp::kEq;
      return true;
    case BinaryOp::kLt:
      *flipped = BinaryOp::kGt;
      return true;
    case BinaryOp::kLe:
      *flipped = BinaryOp::kGe;
      return true;
    case BinaryOp::kGt:
      *flipped = BinaryOp::kLt;
      return true;
    case BinaryOp::kGe:
      *flipped = BinaryOp::kLe;
      return true;
    default:
      return false;
  }
}

// `column op literal` (a flipped one turned around) or `column BETWEEN literal AND literal`, which
// is what a row group's min/max can refute; null for any other conjunct.
ExprPtr PrunableForm(const BoundExpr& conjunct) {
  BinaryOp flipped = BinaryOp::kEq;
  if (conjunct.kind == ExprKind::kBinary && Flip(conjunct.op, &flipped)) {
    const BoundExpr& l = *conjunct.children[0];
    const BoundExpr& r = *conjunct.children[1];
    if (l.kind == ExprKind::kColumn && IsRealLiteral(r)) return conjunct.Clone();
    if (r.kind == ExprKind::kColumn && IsRealLiteral(l)) {
      auto turned = MakeBinary(flipped, r.Clone(), l.Clone());
      return turned.ok() ? std::move(*turned) : nullptr;
    }
  }
  if (conjunct.kind == ExprKind::kBetween && conjunct.children[0]->kind == ExprKind::kColumn &&
      IsRealLiteral(*conjunct.children[1]) && IsRealLiteral(*conjunct.children[2])) {
    return conjunct.Clone();
  }
  return nullptr;
}

arrow::Result<PhysicalPlan> ConvertScan(const LogicalScan& scan, bool prune_row_groups) {
  auto node = Make<PhysicalScan>(scan);
  node->table = scan.table;
  node->table_name = scan.table_name;
  if (scan.table != nullptr) node->files = scan.table->files;
  for (const int c : scan.columns) {
    const size_t index = static_cast<size_t>(c);
    const std::optional<LogicalType>& type = scan.table_column_types[index];
    if (!type.has_value()) {
      return arrow::Status::Invalid("planner error at 1:1: scan of " + scan.table_name +
                                    " lists column " + scan.table_column_names[index] +
                                    ", which has no supported type");
    }
    node->columns.push_back(PhysicalColumn{c, scan.table_column_names[index], *type});
  }
  node->predicate = AndAll(CloneAll(scan.filters));
  if (prune_row_groups) {
    for (const ExprPtr& filter : scan.filters) {
      std::vector<ExprPtr> conjuncts;
      FlattenAnd(filter->Clone(), &conjuncts);
      for (const ExprPtr& c : conjuncts) {
        if (ExprPtr prunable = PrunableForm(*c))
          node->prune_conjuncts.push_back(std::move(prunable));
      }
    }
  }
  return PhysicalPlan(std::move(node));
}

arrow::Result<PhysicalPlan> Convert(const LogicalNode& logical, bool prune_row_groups);

PhysicalPlan ConvertTopN(const LogicalSort& sort, const LogicalLimit& limit, PhysicalPlan child) {
  auto top = std::make_unique<PhysicalTopN>();
  top->keys = CloneKeys(sort.keys);
  top->n = limit.n;
  top->estimated_rows = std::min(EstimateRows(sort), static_cast<double>(limit.n));
  top->output_types = sort.output_types();
  top->child = std::move(child);
  return top;
}

arrow::Result<PhysicalPlan> Convert(const LogicalNode& logical, bool prune_row_groups) {
  switch (logical.kind) {
    case LogicalKind::kScan:
      return ConvertScan(static_cast<const LogicalScan&>(logical), prune_row_groups);
    case LogicalKind::kFilter: {
      const auto& filter = static_cast<const LogicalFilter&>(logical);
      auto node = Make<PhysicalFilter>(logical);
      node->predicate = filter.predicate->Clone();
      ARROW_ASSIGN_OR_RAISE(node->child, Convert(*filter.child, prune_row_groups));
      return PhysicalPlan(std::move(node));
    }
    case LogicalKind::kProject: {
      const auto& project = static_cast<const LogicalProject&>(logical);
      auto node = Make<PhysicalProject>(logical);
      node->exprs = CloneAll(project.exprs);
      node->names = project.names;
      ARROW_ASSIGN_OR_RAISE(node->child, Convert(*project.child, prune_row_groups));
      return PhysicalPlan(std::move(node));
    }
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(logical);
      auto node = Make<PhysicalHashJoin>(logical);
      node->probe_keys = CloneAll(join.left_keys);
      node->build_keys = CloneAll(join.right_keys);
      if (join.residual != nullptr) node->residual = join.residual->Clone();
      ARROW_ASSIGN_OR_RAISE(node->probe, Convert(*join.left, prune_row_groups));
      ARROW_ASSIGN_OR_RAISE(node->build, Convert(*join.right, prune_row_groups));
      const size_t probe_width = node->probe->output_types.size();
      const size_t build_width = node->build->output_types.size();
      for (size_t i = 0; i < probe_width; ++i) {
        node->output_columns.push_back(JoinOutputColumn{false, static_cast<int>(i)});
      }
      for (size_t i = 0; i < build_width; ++i) {
        node->output_columns.push_back(JoinOutputColumn{true, static_cast<int>(i)});
      }
      return PhysicalPlan(std::move(node));
    }
    case LogicalKind::kAggregate: {
      const auto& aggregate = static_cast<const LogicalAggregate&>(logical);
      auto node = Make<PhysicalHashAggregate>(logical);
      node->groups = CloneAll(aggregate.groups);
      for (const AggregateExpr& a : aggregate.aggregates) {
        AggregateExpr copy;
        copy.func = a.func;
        if (a.arg != nullptr) copy.arg = a.arg->Clone();
        copy.result_type = a.result_type;
        node->aggregates.push_back(std::move(copy));
      }
      ARROW_ASSIGN_OR_RAISE(node->child, Convert(*aggregate.child, prune_row_groups));
      return PhysicalPlan(std::move(node));
    }
    case LogicalKind::kSort: {
      const auto& sort = static_cast<const LogicalSort&>(logical);
      auto node = Make<PhysicalSort>(logical);
      node->keys = CloneKeys(sort.keys);
      ARROW_ASSIGN_OR_RAISE(node->child, Convert(*sort.child, prune_row_groups));
      return PhysicalPlan(std::move(node));
    }
    case LogicalKind::kLimit: {
      const auto& limit = static_cast<const LogicalLimit&>(logical);
      if (limit.child->kind == LogicalKind::kSort) {
        const auto& sort = static_cast<const LogicalSort&>(*limit.child);
        ARROW_ASSIGN_OR_RAISE(PhysicalPlan input, Convert(*sort.child, prune_row_groups));
        return ConvertTopN(sort, limit, std::move(input));
      }
      if (limit.child->kind == LogicalKind::kProject &&
          static_cast<const LogicalProject&>(*limit.child).child->kind == LogicalKind::kSort) {
        // LIMIT over the Project that trims hidden sort columns: the Project goes on top.
        const auto& project = static_cast<const LogicalProject&>(*limit.child);
        const auto& sort = static_cast<const LogicalSort&>(*project.child);
        auto node = std::make_unique<PhysicalProject>();
        node->estimated_rows = EstimateRows(limit);
        node->output_types = limit.output_types();
        node->exprs = CloneAll(project.exprs);
        node->names = project.names;
        ARROW_ASSIGN_OR_RAISE(PhysicalPlan input, Convert(*sort.child, prune_row_groups));
        node->child = ConvertTopN(sort, limit, std::move(input));
        return PhysicalPlan(std::move(node));
      }
      auto node = Make<PhysicalLimit>(logical);
      node->n = limit.n;
      ARROW_ASSIGN_OR_RAISE(node->child, Convert(*limit.child, prune_row_groups));
      return PhysicalPlan(std::move(node));
    }
    case LogicalKind::kCrossProduct:
      return arrow::Status::Invalid(
          "planner error at 1:1: a cross product reached the physical planner");
  }
  return arrow::Status::Invalid("unknown logical node");
}

std::vector<PhysicalNode*> MutableChildren(PhysicalNode& node) {
  switch (node.kind) {
    case PhysicalKind::kScan:
      return {};
    case PhysicalKind::kFilter:
      return {static_cast<PhysicalFilter&>(node).child.get()};
    case PhysicalKind::kProject:
      return {static_cast<PhysicalProject&>(node).child.get()};
    case PhysicalKind::kHashJoin: {
      auto& join = static_cast<PhysicalHashJoin&>(node);
      return {join.probe.get(), join.build.get()};
    }
    case PhysicalKind::kHashAggregate:
      return {static_cast<PhysicalHashAggregate&>(node).child.get()};
    case PhysicalKind::kSort:
      return {static_cast<PhysicalSort&>(node).child.get()};
    case PhysicalKind::kTopN:
      return {static_cast<PhysicalTopN&>(node).child.get()};
    case PhysicalKind::kLimit:
      return {static_cast<PhysicalLimit&>(node).child.get()};
  }
  return {};
}

void AssignIds(PhysicalNode& node, int* next) {
  node.id = (*next)++;
  for (PhysicalNode* child : MutableChildren(node)) AssignIds(*child, next);
}

}  // namespace

arrow::Result<PhysicalPlan> PlanPhysical(const LogicalPlan& plan, const PlannerOptions& options) {
  ARROW_RETURN_NOT_OK(ValidateRuleNames(options.disabled_rules));
  ARROW_ASSIGN_OR_RAISE(PhysicalPlan physical,
                        Convert(*plan, IsRuleEnabled(options, "prune_row_groups")));
  int next = 0;
  AssignIds(*physical, &next);
  return physical;
}

}  // namespace quarry
