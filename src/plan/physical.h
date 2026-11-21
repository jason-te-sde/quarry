#ifndef QUARRY_PLAN_PHYSICAL_H_
#define QUARRY_PLAN_PHYSICAL_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "expr/bound_expr.h"
#include "plan/logical.h"
#include "types/logical_type.h"

namespace quarry {

struct TableInfo;  // binder/catalog.h

enum class PhysicalKind : uint8_t {
  kScan,
  kFilter,
  kProject,
  kHashJoin,
  kHashAggregate,
  kSort,
  kTopN,
  kLimit
};

// A physical plan is a tree of descriptors: what to run, with what expressions and on which
// estimates. The operators that run it are built from it in src/exec. Column references inside a
// node index its child's output (a join's residual indexes probe output followed by build
// output).
struct PhysicalNode {
  explicit PhysicalNode(PhysicalKind k) : kind(k) {}
  virtual ~PhysicalNode() = default;
  PhysicalNode(const PhysicalNode&) = delete;
  PhysicalNode& operator=(const PhysicalNode&) = delete;

  const PhysicalKind kind;
  int id = 0;                 // pre-order position, from 0; a join lists its probe side first
  double estimated_rows = 0;  // from planner/cardinality.h
  std::vector<LogicalType> output_types;

  // The inputs, in the order Explain prints them.
  virtual std::vector<const PhysicalNode*> children() const = 0;
};

using PhysicalPlan = std::unique_ptr<PhysicalNode>;

// "Scan", "Filter", "Project", "HashJoin", "HashAggregate", "Sort", "TopN", "Limit".
const char* PhysicalKindName(PhysicalKind kind);

struct PhysicalColumn {
  int table_column = 0;  // index into the table's columns (TableInfo::columns)
  std::string name;
  LogicalType type;
};

// Reads `columns` of a table's files. `predicate` (null for none) is the conjunction of the scan's
// filters and indexes the scan's output; the scan applies it. `prune_conjuncts` is the subset of
// them that a row group's min/max statistics can refute, each in the form `column op literal` with
// op one of = < <= > >= (a flipped comparison is turned around), or `column BETWEEN literal AND
// literal`; empty when prune_row_groups is disabled. A row group where one of them cannot hold for
// any row is skipped.
struct PhysicalScan : PhysicalNode {
  PhysicalScan() : PhysicalNode(PhysicalKind::kScan) {}
  const TableInfo* table = nullptr;  // carries the files' statistics
  std::string table_name;
  std::vector<std::filesystem::path> files;
  std::vector<PhysicalColumn> columns;
  ExprPtr predicate;
  std::vector<ExprPtr> prune_conjuncts;

  std::vector<const PhysicalNode*> children() const override { return {}; }
};

struct PhysicalFilter : PhysicalNode {
  PhysicalFilter() : PhysicalNode(PhysicalKind::kFilter) {}
  PhysicalPlan child;
  ExprPtr predicate;

  std::vector<const PhysicalNode*> children() const override { return {child.get()}; }
};

struct PhysicalProject : PhysicalNode {
  PhysicalProject() : PhysicalNode(PhysicalKind::kProject) {}
  PhysicalPlan child;
  std::vector<ExprPtr> exprs;
  std::vector<std::string> names;

  std::vector<const PhysicalNode*> children() const override { return {child.get()}; }
};

// An inner equi-join: the hash table is built from `build`, `probe` streams past it.
// probe_keys[i] = build_keys[i]; each key indexes its own side. `residual` (may be null) is
// checked on matches and indexes probe output followed by build output. The join emits
// `output_columns`, in order, each taken from one side.
struct JoinOutputColumn {
  bool from_build = false;
  int index = 0;  // into that side's output
};

struct PhysicalHashJoin : PhysicalNode {
  PhysicalHashJoin() : PhysicalNode(PhysicalKind::kHashJoin) {}
  PhysicalPlan build;
  PhysicalPlan probe;
  std::vector<ExprPtr> build_keys;
  std::vector<ExprPtr> probe_keys;
  ExprPtr residual;
  std::vector<JoinOutputColumn> output_columns;

  std::vector<const PhysicalNode*> children() const override { return {probe.get(), build.get()}; }
};

struct PhysicalHashAggregate : PhysicalNode {
  PhysicalHashAggregate() : PhysicalNode(PhysicalKind::kHashAggregate) {}
  PhysicalPlan child;
  std::vector<ExprPtr> groups;
  std::vector<AggregateExpr> aggregates;

  std::vector<const PhysicalNode*> children() const override { return {child.get()}; }
};

struct PhysicalSort : PhysicalNode {
  PhysicalSort() : PhysicalNode(PhysicalKind::kSort) {}
  PhysicalPlan child;
  std::vector<SortKey> keys;

  std::vector<const PhysicalNode*> children() const override { return {child.get()}; }
};

// ORDER BY ... LIMIT n in one node: the first n rows of the sorted input.
struct PhysicalTopN : PhysicalNode {
  PhysicalTopN() : PhysicalNode(PhysicalKind::kTopN) {}
  PhysicalPlan child;
  std::vector<SortKey> keys;
  int64_t n = 0;

  std::vector<const PhysicalNode*> children() const override { return {child.get()}; }
};

struct PhysicalLimit : PhysicalNode {
  PhysicalLimit() : PhysicalNode(PhysicalKind::kLimit) {}
  PhysicalPlan child;
  int64_t n = 0;

  std::vector<const PhysicalNode*> children() const override { return {child.get()}; }
};

}  // namespace quarry

#endif  // QUARRY_PLAN_PHYSICAL_H_
