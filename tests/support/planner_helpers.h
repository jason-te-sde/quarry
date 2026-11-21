#ifndef QUARRY_TESTS_SUPPORT_PLANNER_HELPERS_H_
#define QUARRY_TESTS_SUPPORT_PLANNER_HELPERS_H_

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "binder/binder.h"
#include "binder/catalog.h"
#include "plan/logical.h"
#include "planner/rules.h"

namespace quarry::testing {

// The SF0.01 TPC-H tables, registered once.
inline const Catalog& TpchCatalog() {
  static const Catalog* catalog = [] {
    auto* c = new Catalog();
    const arrow::Status status = c->AddDirectory("tests/data/tpch-sf0.01");
    EXPECT_TRUE(status.ok()) << status.ToString();
    return c;
  }();
  return *catalog;
}

inline std::string ReadTextFile(const std::string& path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline std::string TpchQuery(int number) {
  return ReadTextFile("tests/tpch/queries/q" + std::to_string(number) + ".sql");
}

inline LogicalPlan MustBind(const std::string& sql) {
  auto plan = ParseAndBind(sql, TpchCatalog());
  EXPECT_TRUE(plan.ok()) << sql << "\n" << plan.status().ToString();
  if (!plan.ok()) return nullptr;
  return std::move(*plan);
}

// Options that disable every rule except `enabled` (which may be empty).
inline PlannerOptions OnlyRules(const std::set<std::string>& enabled) {
  PlannerOptions options;
  for (const std::string_view rule : kRuleNames) {
    if (enabled.count(std::string(rule)) == 0) options.disabled_rules.insert(std::string(rule));
  }
  return options;
}

inline PlannerOptions Disabling(const std::set<std::string>& rules) {
  PlannerOptions options;
  options.disabled_rules = rules;
  return options;
}

inline LogicalPlan MustOptimize(const std::string& sql, const PlannerOptions& options) {
  LogicalPlan bound = MustBind(sql);
  if (bound == nullptr) return nullptr;
  auto plan = Optimize(std::move(bound), options);
  EXPECT_TRUE(plan.ok()) << sql << "\n" << plan.status().ToString();
  if (!plan.ok()) return nullptr;
  return std::move(*plan);
}

// The children of a logical node, in order.
inline std::vector<const LogicalNode*> ChildrenOf(const LogicalNode& node) {
  switch (node.kind) {
    case LogicalKind::kScan:
      return {};
    case LogicalKind::kFilter:
      return {static_cast<const LogicalFilter&>(node).child.get()};
    case LogicalKind::kProject:
      return {static_cast<const LogicalProject&>(node).child.get()};
    case LogicalKind::kAggregate:
      return {static_cast<const LogicalAggregate&>(node).child.get()};
    case LogicalKind::kSort:
      return {static_cast<const LogicalSort&>(node).child.get()};
    case LogicalKind::kLimit:
      return {static_cast<const LogicalLimit&>(node).child.get()};
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(node);
      return {join.left.get(), join.right.get()};
    }
    case LogicalKind::kCrossProduct: {
      std::vector<const LogicalNode*> out;
      for (const LogicalPlan& c : static_cast<const LogicalCrossProduct&>(node).children) {
        out.push_back(c.get());
      }
      return out;
    }
  }
  return {};
}

// Every node of kind `kind`, pre-order.
inline void CollectKind(const LogicalNode& node, LogicalKind kind,
                        std::vector<const LogicalNode*>* out) {
  if (node.kind == kind) out->push_back(&node);
  for (const LogicalNode* child : ChildrenOf(node)) CollectKind(*child, kind, out);
}

inline std::vector<const LogicalNode*> NodesOfKind(const LogicalNode& root, LogicalKind kind) {
  std::vector<const LogicalNode*> out;
  CollectKind(root, kind, &out);
  return out;
}

inline const LogicalScan* FindScan(const LogicalNode& root, const std::string& table) {
  for (const LogicalNode* node : NodesOfKind(root, LogicalKind::kScan)) {
    const auto* scan = static_cast<const LogicalScan*>(node);
    if (scan->table_name == table) return scan;
  }
  return nullptr;
}

// The scans in left-to-right order.
inline std::vector<std::string> ScanOrder(const LogicalNode& root) {
  std::vector<std::string> out;
  for (const LogicalNode* node : NodesOfKind(root, LogicalKind::kScan)) {
    out.push_back(static_cast<const LogicalScan*>(node)->table_name);
  }
  return out;
}

// "table.column" of output column `index` of `node`, followed down through filters, sorts, limits
// and joins to the scan; "?" when it does not come from a scan column.
inline std::string TraceColumn(const LogicalNode& node, int index) {
  switch (node.kind) {
    case LogicalKind::kScan: {
      const auto& scan = static_cast<const LogicalScan&>(node);
      return scan.table_name + "." +
             scan.table_column_names[static_cast<size_t>(scan.columns[static_cast<size_t>(index)])];
    }
    case LogicalKind::kFilter:
      return TraceColumn(*static_cast<const LogicalFilter&>(node).child, index);
    case LogicalKind::kSort:
      return TraceColumn(*static_cast<const LogicalSort&>(node).child, index);
    case LogicalKind::kLimit:
      return TraceColumn(*static_cast<const LogicalLimit&>(node).child, index);
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(node);
      const int left_width = static_cast<int>(join.left->output_types().size());
      return index < left_width ? TraceColumn(*join.left, index)
                                : TraceColumn(*join.right, index - left_width);
    }
    default:
      return "?";
  }
}

// The base column a key expression is (looking through casts), else "?".
inline std::string KeyName(const LogicalNode& side, const BoundExpr& key) {
  const BoundExpr* e = &key;
  while (e->kind == ExprKind::kCast) e = e->children[0].get();
  return e->kind == ExprKind::kColumn ? TraceColumn(side, e->column_index) : "?";
}

// Checks that every column reference of every node is inside its child's output and has that
// column's type, and that a join's two key lists agree in length and type.
inline void ExpectWellFormedExpr(const BoundExpr& e, const std::vector<LogicalType>& input,
                                 const std::string& where) {
  if (e.kind == ExprKind::kColumn) {
    ASSERT_GE(e.column_index, 0) << where;
    ASSERT_LT(static_cast<size_t>(e.column_index), input.size()) << where << " " << e.ToString();
    EXPECT_EQ(e.type, input[static_cast<size_t>(e.column_index)]) << where << " " << e.ToString();
  }
  for (const ExprPtr& c : e.children) ExpectWellFormedExpr(*c, input, where);
}

inline void ExpectWellFormed(const LogicalNode& node) {
  const std::string where = node.ToString();
  const auto check = [&](const ExprPtr& e, const std::vector<LogicalType>& input) {
    if (e != nullptr) ExpectWellFormedExpr(*e, input, where);
  };
  switch (node.kind) {
    case LogicalKind::kScan: {
      const auto& scan = static_cast<const LogicalScan&>(node);
      for (const ExprPtr& f : scan.filters) check(f, scan.output_types());
      break;
    }
    case LogicalKind::kFilter: {
      const auto& n = static_cast<const LogicalFilter&>(node);
      check(n.predicate, n.child->output_types());
      break;
    }
    case LogicalKind::kProject: {
      const auto& n = static_cast<const LogicalProject&>(node);
      for (const ExprPtr& e : n.exprs) check(e, n.child->output_types());
      break;
    }
    case LogicalKind::kAggregate: {
      const auto& n = static_cast<const LogicalAggregate&>(node);
      for (const ExprPtr& e : n.groups) check(e, n.child->output_types());
      for (const AggregateExpr& a : n.aggregates) check(a.arg, n.child->output_types());
      break;
    }
    case LogicalKind::kSort: {
      const auto& n = static_cast<const LogicalSort&>(node);
      for (const SortKey& k : n.keys) check(k.expr, n.child->output_types());
      break;
    }
    case LogicalKind::kJoin: {
      const auto& n = static_cast<const LogicalJoin&>(node);
      EXPECT_EQ(n.left_keys.size(), n.right_keys.size()) << where;
      for (size_t i = 0; i < n.left_keys.size() && i < n.right_keys.size(); ++i) {
        check(n.left_keys[i], n.left->output_types());
        check(n.right_keys[i], n.right->output_types());
        EXPECT_EQ(n.left_keys[i]->type, n.right_keys[i]->type) << where;
      }
      std::vector<LogicalType> both = n.left->output_types();
      const std::vector<LogicalType> right = n.right->output_types();
      both.insert(both.end(), right.begin(), right.end());
      check(n.residual, both);
      break;
    }
    case LogicalKind::kLimit:
    case LogicalKind::kCrossProduct:
      break;
  }
  for (const LogicalNode* child : ChildrenOf(node)) ExpectWellFormed(*child);
}

}  // namespace quarry::testing

#endif  // QUARRY_TESTS_SUPPORT_PLANNER_HELPERS_H_
