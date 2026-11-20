#include "planner/cardinality.h"

#include <arrow/scalar.h>

#include <algorithm>
#include <cstddef>

#include "binder/catalog.h"

namespace quarry {
namespace {

constexpr double kEqualSelectivity = 0.1;
constexpr double kNotEqualSelectivity = 0.9;
constexpr double kRangeSelectivity = 0.3;
constexpr double kInPerValue = 0.1;
constexpr double kInCap = 0.5;
constexpr double kIsNullSelectivity = 0.1;
constexpr double kIsNotNullSelectivity = 0.9;
constexpr double kExtraKeyPairSelectivity = 0.1;
constexpr double kGroupFraction = 0.1;

double RowsOfTable(const LogicalScan& scan) {
  return scan.table == nullptr ? 0.0 : static_cast<double>(scan.table->row_count);
}

// The base table that output column `index` of `node` comes from, as its row count; false when the
// column is computed or its origin is not a single scan.
bool TraceBaseRows(const LogicalNode& node, int index, double* rows) {
  switch (node.kind) {
    case LogicalKind::kScan:
      *rows = RowsOfTable(static_cast<const LogicalScan&>(node));
      return true;
    case LogicalKind::kFilter:
      return TraceBaseRows(*static_cast<const LogicalFilter&>(node).child, index, rows);
    case LogicalKind::kSort:
      return TraceBaseRows(*static_cast<const LogicalSort&>(node).child, index, rows);
    case LogicalKind::kLimit:
      return TraceBaseRows(*static_cast<const LogicalLimit&>(node).child, index, rows);
    case LogicalKind::kProject: {
      const auto& project = static_cast<const LogicalProject&>(node);
      const BoundExpr& e = *project.exprs[static_cast<size_t>(index)];
      return e.kind == ExprKind::kColumn && TraceBaseRows(*project.child, e.column_index, rows);
    }
    case LogicalKind::kAggregate: {
      const auto& aggregate = static_cast<const LogicalAggregate&>(node);
      if (static_cast<size_t>(index) >= aggregate.groups.size()) return false;
      const BoundExpr& e = *aggregate.groups[static_cast<size_t>(index)];
      return e.kind == ExprKind::kColumn && TraceBaseRows(*aggregate.child, e.column_index, rows);
    }
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(node);
      const int left_width = static_cast<int>(join.left->output_types().size());
      return index < left_width ? TraceBaseRows(*join.left, index, rows)
                                : TraceBaseRows(*join.right, index - left_width, rows);
    }
    case LogicalKind::kCrossProduct: {
      int offset = 0;
      for (const LogicalPlan& child : static_cast<const LogicalCrossProduct&>(node).children) {
        const int width = static_cast<int>(child->output_types().size());
        if (index < offset + width) return TraceBaseRows(*child, index - offset, rows);
        offset += width;
      }
      return false;
    }
  }
  return false;
}

// The first column reference of an expression, or -1.
int FirstColumn(const BoundExpr& e) {
  if (e.kind == ExprKind::kColumn) return e.column_index;
  for (const ExprPtr& child : e.children) {
    const int c = FirstColumn(*child);
    if (c >= 0) return c;
  }
  return -1;
}

}  // namespace

double EstimateSelectivity(const BoundExpr& e) {
  switch (e.kind) {
    case ExprKind::kBinary:
      switch (e.op) {
        case BinaryOp::kEq:
          return kEqualSelectivity;
        case BinaryOp::kNe:
          return kNotEqualSelectivity;
        case BinaryOp::kLt:
        case BinaryOp::kLe:
        case BinaryOp::kGt:
        case BinaryOp::kGe:
          return kRangeSelectivity;
        case BinaryOp::kAnd:
          return EstimateSelectivity(*e.children[0]) * EstimateSelectivity(*e.children[1]);
        case BinaryOp::kOr: {
          const double a = EstimateSelectivity(*e.children[0]);
          const double b = EstimateSelectivity(*e.children[1]);
          return a + b - a * b;
        }
        default:
          return 1.0;
      }
    case ExprKind::kNot:
      return 1.0 - EstimateSelectivity(*e.children[0]);
    case ExprKind::kBetween:
      return kRangeSelectivity;
    case ExprKind::kInList:
      return std::min(kInCap, kInPerValue * static_cast<double>(e.in_list.size()));
    case ExprKind::kIsNull:
      return e.negated ? kIsNotNullSelectivity : kIsNullSelectivity;
    case ExprKind::kLiteral:
      // A constant predicate keeps everything or nothing.
      if (e.literal == nullptr || !e.literal->is_valid) return 0.0;
      if (e.literal->type->id() == arrow::Type::BOOL) {
        return static_cast<const arrow::BooleanScalar&>(*e.literal).value ? 1.0 : 0.0;
      }
      return 1.0;
    default:
      return 1.0;
  }
}

double EstimateJoinRows(double left_rows, double right_rows,
                        const std::vector<double>& key_base_rows) {
  double rows = left_rows * right_rows;
  if (key_base_rows.empty()) return rows;
  rows /= std::max(1.0, key_base_rows[0]);
  for (size_t i = 1; i < key_base_rows.size(); ++i) rows *= kExtraKeyPairSelectivity;
  return rows;
}

double KeyBaseRows(const LogicalNode& side, const BoundExpr& key) {
  const int column = FirstColumn(key);
  double rows = 0;
  if (column >= 0 && TraceBaseRows(side, column, &rows)) return rows;
  return EstimateRows(side);
}

double EstimateRows(const LogicalNode& node) {
  switch (node.kind) {
    case LogicalKind::kScan: {
      const auto& scan = static_cast<const LogicalScan&>(node);
      double rows = RowsOfTable(scan);
      for (const ExprPtr& f : scan.filters) rows *= EstimateSelectivity(*f);
      return rows;
    }
    case LogicalKind::kFilter: {
      const auto& filter = static_cast<const LogicalFilter&>(node);
      return EstimateRows(*filter.child) * EstimateSelectivity(*filter.predicate);
    }
    case LogicalKind::kProject:
      return EstimateRows(*static_cast<const LogicalProject&>(node).child);
    case LogicalKind::kSort:
      return EstimateRows(*static_cast<const LogicalSort&>(node).child);
    case LogicalKind::kLimit: {
      const auto& limit = static_cast<const LogicalLimit&>(node);
      return std::min(EstimateRows(*limit.child), static_cast<double>(limit.n));
    }
    case LogicalKind::kAggregate: {
      const auto& aggregate = static_cast<const LogicalAggregate&>(node);
      if (aggregate.groups.empty()) return 1.0;
      return std::max(1.0, EstimateRows(*aggregate.child) * kGroupFraction);
    }
    case LogicalKind::kCrossProduct: {
      double rows = 1.0;
      for (const LogicalPlan& child : static_cast<const LogicalCrossProduct&>(node).children) {
        rows *= EstimateRows(*child);
      }
      return rows;
    }
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(node);
      std::vector<double> bases;
      bases.reserve(join.left_keys.size());
      for (size_t i = 0; i < join.left_keys.size(); ++i) {
        bases.push_back(std::min(KeyBaseRows(*join.left, *join.left_keys[i]),
                                 KeyBaseRows(*join.right, *join.right_keys[i])));
      }
      double rows = EstimateJoinRows(EstimateRows(*join.left), EstimateRows(*join.right), bases);
      if (join.residual != nullptr) rows *= EstimateSelectivity(*join.residual);
      return rows;
    }
  }
  return 0.0;
}

}  // namespace quarry
