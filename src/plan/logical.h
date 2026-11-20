#ifndef QUARRY_PLAN_LOGICAL_H_
#define QUARRY_PLAN_LOGICAL_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "expr/bound_expr.h"
#include "types/logical_type.h"
#include "types/type_rules.h"

namespace quarry {

// Defined in binder/catalog.h. The plan layer only keeps a pointer to it, so that the scan can be
// traced back to its files; everything else the plan needs about the table is copied below.
struct TableInfo;

enum class LogicalKind : uint8_t {
  kScan,
  kFilter,
  kProject,
  kCrossProduct,
  kJoin,
  kAggregate,
  kSort,
  kLimit
};

struct LogicalNode;
using LogicalPlan = std::unique_ptr<LogicalNode>;

// A node of the logical plan. Column references (BoundExpr::kColumn) inside a node index the
// concatenated output of its children: a Filter's predicate indexes its child's output, a Join's
// residual indexes left output followed by right output.
struct LogicalNode {
  explicit LogicalNode(LogicalKind k) : kind(k) {}
  virtual ~LogicalNode() = default;
  LogicalNode(const LogicalNode&) = delete;
  LogicalNode& operator=(const LogicalNode&) = delete;

  const LogicalKind kind;

  // The types of the columns this node produces, in order.
  virtual std::vector<LogicalType> output_types() const = 0;
  // One line per node, children indented two spaces further, every line ended by '\n'.
  virtual std::string ToString(int indent = 0) const = 0;
};

// A Parquet table read. `columns` lists the table columns that are produced, in output order, as
// indices into table_column_names / table_column_types; `filters` are conjuncts evaluated on the
// scan's own output (index = position in `columns`). A column of an unsupported type has no type
// and can never be listed in `columns`.
struct LogicalScan : LogicalNode {
  LogicalScan() : LogicalNode(LogicalKind::kScan) {}
  const TableInfo* table = nullptr;
  std::string table_name;
  std::vector<std::string> table_column_names;
  std::vector<std::optional<LogicalType>> table_column_types;
  std::vector<int> columns;
  std::vector<ExprPtr> filters;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

struct LogicalFilter : LogicalNode {
  LogicalFilter() : LogicalNode(LogicalKind::kFilter) {}
  LogicalPlan child;
  ExprPtr predicate;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

struct LogicalProject : LogicalNode {
  LogicalProject() : LogicalNode(LogicalKind::kProject) {}
  LogicalPlan child;
  std::vector<ExprPtr> exprs;
  std::vector<std::string> names;  // one per expression

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

// The comma join of FROM a, b, c: all children side by side. The binder emits it as written; the
// planner turns it into joins using the WHERE equalities.
struct LogicalCrossProduct : LogicalNode {
  LogicalCrossProduct() : LogicalNode(LogicalKind::kCrossProduct) {}
  std::vector<LogicalPlan> children;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

// An inner join written as JOIN ... ON. left_keys[i] = right_keys[i] for every i; each key
// expression indexes its own side's output, and the two have the same type. `residual` (may be
// null) is the rest of the ON condition, indexing left output followed by right output.
struct LogicalJoin : LogicalNode {
  LogicalJoin() : LogicalNode(LogicalKind::kJoin) {}
  LogicalPlan left;
  LogicalPlan right;
  std::vector<ExprPtr> left_keys;
  std::vector<ExprPtr> right_keys;
  ExprPtr residual;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

struct AggregateExpr {
  AggFunc func = AggFunc::kCountStar;
  ExprPtr arg;  // null for COUNT(*)
  LogicalType result_type;
};

// Outputs the group expressions, then the aggregates.
struct LogicalAggregate : LogicalNode {
  LogicalAggregate() : LogicalNode(LogicalKind::kAggregate) {}
  LogicalPlan child;
  std::vector<ExprPtr> groups;
  std::vector<AggregateExpr> aggregates;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

struct SortKey {
  ExprPtr expr;
  bool ascending = true;
  bool nulls_first = false;
};

struct LogicalSort : LogicalNode {
  LogicalSort() : LogicalNode(LogicalKind::kSort) {}
  LogicalPlan child;
  std::vector<SortKey> keys;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

struct LogicalLimit : LogicalNode {
  LogicalLimit() : LogicalNode(LogicalKind::kLimit) {}
  LogicalPlan child;
  int64_t n = 0;

  std::vector<LogicalType> output_types() const override;
  std::string ToString(int indent = 0) const override;
};

}  // namespace quarry

#endif  // QUARRY_PLAN_LOGICAL_H_
