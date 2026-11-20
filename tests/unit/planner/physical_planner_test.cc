#include "planner/physical_planner.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <set>
#include <string>
#include <vector>

#include "plan/explain.h"
#include "support/planner_helpers.h"

namespace quarry {
namespace {

using testing::Disabling;
using ::testing::HasSubstr;
using testing::MustOptimize;
using testing::TpchQuery;

PhysicalPlan MustPlan(const std::string& sql, const PlannerOptions& options = PlannerOptions{}) {
  LogicalPlan logical = MustOptimize(sql, options);
  if (logical == nullptr) return nullptr;
  auto physical = PlanPhysical(logical, options);
  EXPECT_TRUE(physical.ok()) << sql << "\n" << physical.status().ToString();
  if (!physical.ok()) return nullptr;
  return std::move(*physical);
}

void Collect(const PhysicalNode& node, std::vector<const PhysicalNode*>* out) {
  out->push_back(&node);
  for (const PhysicalNode* child : node.children()) Collect(*child, out);
}

std::vector<const PhysicalNode*> All(const PhysicalNode& root) {
  std::vector<const PhysicalNode*> out;
  Collect(root, &out);
  return out;
}

const PhysicalScan* FindScan(const PhysicalNode& root, const std::string& table) {
  for (const PhysicalNode* node : All(root)) {
    if (node->kind == PhysicalKind::kScan) {
      const auto* scan = static_cast<const PhysicalScan*>(node);
      if (scan->table_name == table) return scan;
    }
  }
  return nullptr;
}

TEST(PhysicalPlannerTest, IdsAreAPreOrderNumberingFromZero) {
  const PhysicalPlan plan = MustPlan(TpchQuery(5));
  ASSERT_NE(plan, nullptr);
  const auto nodes = All(*plan);
  for (size_t i = 0; i < nodes.size(); ++i) {
    EXPECT_EQ(nodes[i]->id, static_cast<int>(i));
    EXPECT_GE(nodes[i]->estimated_rows, 0.0);
    EXPECT_FALSE(nodes[i]->output_types.empty());
  }
}

TEST(PhysicalPlannerTest, LimitOverSortBecomesTopN) {
  const PhysicalPlan plan = MustPlan(TpchQuery(3));
  ASSERT_NE(plan, nullptr);
  ASSERT_EQ(plan->kind, PhysicalKind::kTopN);
  const auto& top = static_cast<const PhysicalTopN&>(*plan);
  EXPECT_EQ(top.n, 10);
  EXPECT_EQ(top.keys.size(), 2u);
  for (const PhysicalNode* node : All(*plan)) {
    EXPECT_NE(node->kind, PhysicalKind::kSort);
    EXPECT_NE(node->kind, PhysicalKind::kLimit);
  }
  EXPECT_LE(plan->estimated_rows, 10.0);
}

TEST(PhysicalPlannerTest, ATrimmingProjectGoesOnTopOfTheTopN) {
  // ORDER BY a column that is not selected: a hidden sort column, trimmed after the sort.
  const PhysicalPlan plan = MustPlan("SELECT n_name FROM nation ORDER BY n_nationkey DESC LIMIT 3");
  ASSERT_NE(plan, nullptr);
  ASSERT_EQ(plan->kind, PhysicalKind::kProject);
  const auto& project = static_cast<const PhysicalProject&>(*plan);
  ASSERT_EQ(project.child->kind, PhysicalKind::kTopN);
  EXPECT_EQ(static_cast<const PhysicalTopN&>(*project.child).n, 3);
  EXPECT_EQ(plan->output_types.size(), 1u);
}

TEST(PhysicalPlannerTest, SortWithoutLimitAndLimitWithoutSortStayApart) {
  const PhysicalPlan sorted = MustPlan("SELECT n_name FROM nation ORDER BY n_name");
  ASSERT_NE(sorted, nullptr);
  EXPECT_EQ(sorted->kind, PhysicalKind::kSort);
  const PhysicalPlan limited = MustPlan("SELECT n_name FROM nation LIMIT 4");
  ASSERT_NE(limited, nullptr);
  ASSERT_EQ(limited->kind, PhysicalKind::kLimit);
  EXPECT_EQ(static_cast<const PhysicalLimit&>(*limited).n, 4);
}

TEST(PhysicalPlannerTest, Q6ScanCarriesItsPredicateAndFourPruneConjuncts) {
  const PhysicalPlan plan = MustPlan(TpchQuery(6));
  ASSERT_NE(plan, nullptr);
  const PhysicalScan* scan = FindScan(*plan, "lineitem");
  ASSERT_NE(scan, nullptr);
  EXPECT_EQ(scan->columns.size(), 4u);
  EXPECT_FALSE(scan->files.empty());
  EXPECT_NE(scan->table, nullptr);
  EXPECT_NE(scan->predicate, nullptr);
  // shipdate >= and <, discount BETWEEN, quantity <
  EXPECT_EQ(scan->prune_conjuncts.size(), 4u);
  for (const ExprPtr& c : scan->prune_conjuncts) {
    ASSERT_TRUE(c->kind == ExprKind::kBinary || c->kind == ExprKind::kBetween) << c->ToString();
    EXPECT_EQ(c->children[0]->kind, ExprKind::kColumn);
    for (size_t i = 1; i < c->children.size(); ++i) {
      EXPECT_EQ(c->children[i]->kind, ExprKind::kLiteral) << c->ToString();
    }
  }
}

TEST(PhysicalPlannerTest, PruneRowGroupsDisabledListsNoConjuncts) {
  const PhysicalPlan plan = MustPlan(TpchQuery(6), Disabling({"prune_row_groups"}));
  ASSERT_NE(plan, nullptr);
  const PhysicalScan* scan = FindScan(*plan, "lineitem");
  ASSERT_NE(scan, nullptr);
  EXPECT_TRUE(scan->prune_conjuncts.empty());
  EXPECT_NE(scan->predicate, nullptr);  // the filter itself is still applied
}

TEST(PhysicalPlannerTest, AFlippedComparisonIsTurnedAroundAndNonPrunableOnesAreSkipped) {
  const PhysicalPlan plan = MustPlan(
      "SELECT o_orderkey FROM orders WHERE 5 > o_orderkey AND o_orderkey + 1 > 3 "
      "AND o_orderstatus = 'F' OR o_orderkey = 2");
  ASSERT_NE(plan, nullptr);
  // The OR keeps the whole predicate one non-prunable conjunct.
  const PhysicalScan* scan = FindScan(*plan, "orders");
  ASSERT_NE(scan, nullptr);
  EXPECT_TRUE(scan->prune_conjuncts.empty());

  const PhysicalPlan flipped =
      MustPlan("SELECT o_orderkey FROM orders WHERE 5 > o_orderkey AND o_orderkey + 1 > 3");
  ASSERT_NE(flipped, nullptr);
  const PhysicalScan* flipped_scan = FindScan(*flipped, "orders");
  ASSERT_NE(flipped_scan, nullptr);
  ASSERT_EQ(flipped_scan->prune_conjuncts.size(), 1u);
  const BoundExpr& c = *flipped_scan->prune_conjuncts[0];
  EXPECT_EQ(c.op, BinaryOp::kLt);
  EXPECT_EQ(c.children[0]->kind, ExprKind::kColumn);
  EXPECT_EQ(c.children[1]->kind, ExprKind::kLiteral);
}

TEST(PhysicalPlannerTest, HashJoinBuildsFromTheSmallerInputAndMapsItsColumns) {
  const PhysicalPlan plan = MustPlan(TpchQuery(5));
  ASSERT_NE(plan, nullptr);
  int joins = 0;
  for (const PhysicalNode* node : All(*plan)) {
    if (node->kind != PhysicalKind::kHashJoin) continue;
    ++joins;
    const auto& join = static_cast<const PhysicalHashJoin&>(*node);
    EXPECT_LE(join.build->estimated_rows, join.probe->estimated_rows) << Explain(*node);
    EXPECT_EQ(join.build_keys.size(), join.probe_keys.size());
    ASSERT_EQ(join.output_columns.size(),
              join.probe->output_types.size() + join.build->output_types.size());
    // Output order is probe columns, then build columns.
    const size_t probe_width = join.probe->output_types.size();
    for (size_t i = 0; i < join.output_columns.size(); ++i) {
      EXPECT_EQ(join.output_columns[i].from_build, i >= probe_width);
      EXPECT_EQ(static_cast<size_t>(join.output_columns[i].index),
                i >= probe_width ? i - probe_width : i);
    }
  }
  EXPECT_EQ(joins, 5);
}

TEST(PhysicalPlannerTest, UnknownRuleIsRejected) {
  const LogicalPlan logical = MustOptimize("SELECT n_name FROM nation", PlannerOptions{});
  auto physical = PlanPhysical(logical, Disabling({"bogus"}));
  ASSERT_FALSE(physical.ok());
  EXPECT_THAT(physical.status().message(), HasSubstr("bogus"));
}

TEST(ExplainTest, PrintsOneNodePerLineWithIndentIdAndEstimate) {
  const PhysicalPlan plan = MustPlan("SELECT n_name FROM nation WHERE n_regionkey = 2");
  ASSERT_NE(plan, nullptr);
  const std::string text = Explain(*plan);
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    ASSERT_NE(end, std::string::npos);
    lines.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_THAT(lines[0], ::testing::MatchesRegex("Project #0 \\[.*\\] est=[0-9]+"));
  EXPECT_THAT(lines[1], ::testing::MatchesRegex("  Scan #1 \\[.*\\] est=[0-9]+"));
  EXPECT_THAT(lines[1], HasSubstr("nation"));
  EXPECT_THAT(lines[1], HasSubstr("n_name"));
  EXPECT_THAT(lines[1], HasSubstr("est=3"));  // 25 rows * 0.1, rounded
}

TEST(ExplainTest, NamesTheEightNodeKinds) {
  const std::string q3 = Explain(*MustPlan(TpchQuery(3)));
  for (const char* name : {"TopN #0", "HashAggregate", "HashJoin", "Scan", "Project"}) {
    EXPECT_THAT(q3, HasSubstr(name));
  }
  const std::string other =
      Explain(*MustPlan("SELECT n_name FROM nation WHERE n_name <> 'x' ORDER BY n_name"));
  EXPECT_THAT(other, HasSubstr("Sort #"));
  EXPECT_THAT(Explain(*MustPlan("SELECT n_name FROM nation LIMIT 2")), HasSubstr("Limit #0"));
}

TEST(ExplainTest, PrintsNoFilePaths) {
  const std::string text = Explain(*MustPlan(TpchQuery(5)));
  EXPECT_EQ(text.find(".parquet"), std::string::npos);
  EXPECT_EQ(text.find("tests/"), std::string::npos);
}

}  // namespace
}  // namespace quarry
