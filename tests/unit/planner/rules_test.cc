#include "planner/rules.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "support/planner_helpers.h"
#include "types/type_rules.h"

namespace quarry {
namespace {

using testing::Disabling;
using testing::ExpectWellFormed;
using testing::FindScan;
using ::testing::HasSubstr;
using testing::MustBind;
using testing::MustOptimize;
using testing::NodesOfKind;
using testing::OnlyRules;
using testing::TpchQuery;

const LogicalProject& TopProject(const LogicalPlan& plan) {
  EXPECT_EQ(plan->kind, LogicalKind::kProject);
  return static_cast<const LogicalProject&>(*plan);
}

TEST(RulesTest, FoldConstantsTurnsLiteralArithmeticIntoOneLiteral) {
  const std::string sql = "SELECT 0.06 + 0.01 FROM nation";
  const LogicalPlan unfolded = MustBind(sql);
  ASSERT_NE(unfolded, nullptr);
  ASSERT_EQ(TopProject(unfolded).exprs[0]->kind, ExprKind::kBinary);
  const LogicalType expected = TopProject(unfolded).exprs[0]->type;
  EXPECT_EQ(expected.ToString(), "DECIMAL(4,2)");

  const LogicalPlan plan = MustOptimize(sql, OnlyRules({"fold_constants"}));
  ASSERT_NE(plan, nullptr);
  const BoundExpr& folded = *TopProject(plan).exprs[0];
  ASSERT_EQ(folded.kind, ExprKind::kLiteral);
  EXPECT_EQ(folded.type, expected);
  EXPECT_EQ(folded.ToString(), "0.07");
}

TEST(RulesTest, FoldConstantsLeavesColumnExpressionsAndFoldsTheirLiteralParts) {
  const LogicalPlan plan = MustOptimize("SELECT l_orderkey FROM lineitem WHERE l_quantity < 20 + 4",
                                        OnlyRules({"fold_constants"}));
  ASSERT_NE(plan, nullptr);
  const auto filters = NodesOfKind(*plan, LogicalKind::kFilter);
  ASSERT_EQ(filters.size(), 1u);
  const BoundExpr& predicate = *static_cast<const LogicalFilter&>(*filters[0]).predicate;
  ASSERT_EQ(predicate.kind, ExprKind::kBinary);
  EXPECT_EQ(predicate.children[0]->kind, ExprKind::kColumn);
  EXPECT_EQ(predicate.children[1]->kind, ExprKind::kLiteral);
}

TEST(RulesTest, FoldConstantsDisabledKeepsTheExpression) {
  const LogicalPlan plan = MustOptimize("SELECT 0.06 + 0.01 FROM nation", OnlyRules({}));
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(TopProject(plan).exprs[0]->kind, ExprKind::kBinary);
}

TEST(RulesTest, FoldConstantsKeepsAnOverflowForRunTime) {
  // INTEGER + INTEGER overflows. Folding must leave that to the evaluator, not turn it into a
  // planning failure.
  const LogicalPlan plan =
      MustOptimize("SELECT 2147483647 + 1 FROM nation", OnlyRules({"fold_constants"}));
  ASSERT_NE(plan, nullptr);
  EXPECT_EQ(TopProject(plan).exprs[0]->kind, ExprKind::kBinary);
}

TEST(RulesTest, PushFiltersMovesSingleRelationConjunctsIntoTheScan) {
  const std::string sql =
      "SELECT l_orderkey FROM lineitem, orders WHERE l_orderkey = o_orderkey "
      "AND l_shipdate <= date '1998-09-02' AND o_totalprice > 100 AND l_extendedprice > "
      "o_totalprice";
  const LogicalPlan plan = MustOptimize(sql, OnlyRules({"push_filters"}));
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);

  const LogicalScan* lineitem = FindScan(*plan, "lineitem");
  const LogicalScan* orders = FindScan(*plan, "orders");
  ASSERT_NE(lineitem, nullptr);
  ASSERT_NE(orders, nullptr);
  ASSERT_EQ(lineitem->filters.size(), 1u);
  EXPECT_THAT(lineitem->filters[0]->ToString(), HasSubstr("DATE '1998-09-02'"));
  ASSERT_EQ(orders->filters.size(), 1u);

  // The cross-table comparison stays above the join, in a Filter.
  const auto filters = NodesOfKind(*plan, LogicalKind::kFilter);
  ASSERT_EQ(filters.size(), 1u);
  EXPECT_EQ(static_cast<const LogicalFilter&>(*filters[0]).predicate->kind, ExprKind::kBinary);
  EXPECT_EQ(NodesOfKind(*plan, LogicalKind::kJoin).size(), 1u);
}

TEST(RulesTest, PushFiltersDisabledLeavesEveryConjunctAboveTheJoins) {
  const std::string sql =
      "SELECT l_orderkey FROM lineitem, orders WHERE l_orderkey = o_orderkey "
      "AND l_shipdate <= date '1998-09-02'";
  const LogicalPlan plan = MustOptimize(sql, Disabling({"push_filters"}));
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  ASSERT_NE(FindScan(*plan, "lineitem"), nullptr);
  EXPECT_TRUE(FindScan(*plan, "lineitem")->filters.empty());
  EXPECT_EQ(NodesOfKind(*plan, LogicalKind::kFilter).size(), 1u);
}

TEST(RulesTest, PushFiltersReachesThroughAnExplicitJoin) {
  const LogicalPlan plan = MustOptimize(
      "SELECT n_name FROM nation JOIN region ON n_regionkey = r_regionkey "
      "WHERE r_name = 'ASIA'",
      OnlyRules({"push_filters"}));
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  ASSERT_NE(FindScan(*plan, "region"), nullptr);
  EXPECT_EQ(FindScan(*plan, "region")->filters.size(), 1u);
  EXPECT_TRUE(NodesOfKind(*plan, LogicalKind::kFilter).empty());
}

TEST(RulesTest, PruneColumnsMakesQ6ReadFourLineitemColumns) {
  const LogicalPlan plan = MustOptimize(TpchQuery(6), PlannerOptions{});
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  const LogicalScan* scan = FindScan(*plan, "lineitem");
  ASSERT_NE(scan, nullptr);
  std::vector<std::string> names;
  for (const int c : scan->columns)
    names.push_back(scan->table_column_names[static_cast<size_t>(c)]);
  std::sort(names.begin(), names.end());
  EXPECT_THAT(names,
              ::testing::ElementsAre("l_discount", "l_extendedprice", "l_quantity", "l_shipdate"));
}

TEST(RulesTest, PruneColumnsKeepsOneColumnWhenNothingIsNeeded) {
  const LogicalPlan plan = MustOptimize("SELECT count(*) FROM nation", PlannerOptions{});
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  const LogicalScan* scan = FindScan(*plan, "nation");
  ASSERT_NE(scan, nullptr);
  EXPECT_EQ(scan->columns.size(), 1u);
}

TEST(RulesTest, PruneColumnsDisabledKeepsEveryColumn) {
  const LogicalPlan plan = MustOptimize(TpchQuery(6), Disabling({"prune_columns"}));
  ASSERT_NE(plan, nullptr);
  const LogicalScan* scan = FindScan(*plan, "lineitem");
  ASSERT_NE(scan, nullptr);
  EXPECT_EQ(scan->columns.size(), scan->table_column_names.size());
}

TEST(RulesTest, EveryRuleSubsetKeepsThePlanWellFormedAndItsOutputTypes) {
  const std::vector<std::string> rules(kRuleNames.begin(), kRuleNames.end());
  for (const int q : {1, 3, 5, 6}) {
    const std::string sql = TpchQuery(q);
    const LogicalPlan bound = MustBind(sql);
    ASSERT_NE(bound, nullptr);
    const auto expected_types = bound->output_types();
    for (unsigned mask = 0; mask < (1u << rules.size()); ++mask) {
      std::set<std::string> disabled;
      for (size_t i = 0; i < rules.size(); ++i) {
        if ((mask >> i) & 1u) disabled.insert(rules[i]);
      }
      SCOPED_TRACE("Q" + std::to_string(q) + " mask " + std::to_string(mask));
      const LogicalPlan plan = MustOptimize(sql, Disabling(disabled));
      ASSERT_NE(plan, nullptr);
      ExpectWellFormed(*plan);
      EXPECT_EQ(plan->output_types(), expected_types);
      EXPECT_TRUE(NodesOfKind(*plan, LogicalKind::kCrossProduct).empty());
    }
  }
}

TEST(RulesTest, OptimizeRejectsAnUnknownRuleName) {
  auto plan = Optimize(MustBind("SELECT n_name FROM nation"), Disabling({"no_such_rule"}));
  ASSERT_FALSE(plan.ok());
  EXPECT_THAT(plan.status().message(), HasSubstr("no_such_rule"));
}

TEST(ValidateRuleNamesTest, AcceptsTheSixNamesAndTheEmptySet) {
  EXPECT_TRUE(ValidateRuleNames({}).ok());
  EXPECT_TRUE(ValidateRuleNames({"fold_constants", "push_filters", "prune_row_groups",
                                 "prune_columns", "order_joins", "pick_build_side"})
                  .ok());
}

TEST(ValidateRuleNamesTest, RejectsAnUnknownNameAndListsTheValidOnes) {
  const arrow::Status status = ValidateRuleNames({"order_joins", "reorder_joins"});
  ASSERT_TRUE(status.IsInvalid());
  const std::string& message = status.message();
  EXPECT_THAT(message, HasSubstr("reorder_joins"));
  for (const char* name : {"fold_constants", "push_filters", "prune_row_groups", "prune_columns",
                           "order_joins", "pick_build_side"}) {
    EXPECT_THAT(message, HasSubstr(name));
  }
}

TEST(ValidateRuleNamesTest, NamesAreExactMatches) {
  EXPECT_FALSE(ValidateRuleNames({"Order_Joins"}).ok());
  EXPECT_FALSE(ValidateRuleNames({""}).ok());
}

}  // namespace
}  // namespace quarry
