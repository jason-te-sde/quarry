#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "planner/rules.h"
#include "support/planner_helpers.h"

namespace quarry {
namespace {

using testing::Disabling;
using ::testing::ElementsAre;
using testing::ExpectWellFormed;
using ::testing::HasSubstr;
using testing::KeyName;
using testing::MustBind;
using testing::MustOptimize;
using testing::NodesOfKind;
using testing::ScanOrder;
using testing::TpchQuery;

// "a=b" with the two names sorted, for each key pair of each join.
std::multiset<std::string> KeyPairs(const LogicalNode& plan) {
  std::multiset<std::string> out;
  for (const LogicalNode* node : NodesOfKind(plan, LogicalKind::kJoin)) {
    const auto& join = static_cast<const LogicalJoin&>(*node);
    for (size_t i = 0; i < join.left_keys.size(); ++i) {
      std::string a = KeyName(*join.left, *join.left_keys[i]);
      std::string b = KeyName(*join.right, *join.right_keys[i]);
      if (b < a) std::swap(a, b);
      out.insert(a + "=" + b);
    }
  }
  return out;
}

TEST(JoinOrderTest, Q5JoinsAllSixTablesWithoutACrossProduct) {
  const LogicalPlan plan = MustOptimize(TpchQuery(5), PlannerOptions{});
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  EXPECT_EQ(NodesOfKind(*plan, LogicalKind::kScan).size(), 6u);
  EXPECT_EQ(NodesOfKind(*plan, LogicalKind::kJoin).size(), 5u);
  EXPECT_TRUE(NodesOfKind(*plan, LogicalKind::kCrossProduct).empty());
}

TEST(JoinOrderTest, Q5KeepsEveryEqualityAsAJoinKeyNotAResidual) {
  const LogicalPlan plan = MustOptimize(TpchQuery(5), PlannerOptions{});
  ASSERT_NE(plan, nullptr);
  // Six equalities connect the six tables, one of them (c_nationkey = s_nationkey) closing a
  // cycle: all six are keys, none dropped, none left as a residual or a Filter above.
  EXPECT_THAT(
      KeyPairs(*plan),
      ::testing::UnorderedElementsAre(
          "customer.c_custkey=orders.o_custkey", "lineitem.l_orderkey=orders.o_orderkey",
          "lineitem.l_suppkey=supplier.s_suppkey", "customer.c_nationkey=supplier.s_nationkey",
          "nation.n_nationkey=supplier.s_nationkey", "nation.n_regionkey=region.r_regionkey"));
  for (const LogicalNode* node : NodesOfKind(*plan, LogicalKind::kJoin)) {
    EXPECT_EQ(static_cast<const LogicalJoin&>(*node).residual, nullptr);
  }
  EXPECT_TRUE(NodesOfKind(*plan, LogicalKind::kFilter).empty());
}

TEST(JoinOrderTest, Q5StartsFromTheSmallestFilteredRelation) {
  // Without pick_build_side the scans are in join order, left to right.
  const LogicalPlan plan = MustOptimize(TpchQuery(5), Disabling({"pick_build_side"}));
  ASSERT_NE(plan, nullptr);
  const std::vector<std::string> order = ScanOrder(*plan);
  ASSERT_EQ(order.size(), 6u);
  EXPECT_EQ(order[0], "region");
  EXPECT_EQ(order[1], "nation");
  EXPECT_EQ(order[2], "supplier");
  EXPECT_NE(order, (std::vector<std::string>{"customer", "orders", "lineitem", "supplier", "nation",
                                             "region"}));
}

TEST(JoinOrderTest, WithOrderJoinsDisabledTheJoinOrderIsTheFromOrder) {
  const LogicalPlan plan =
      MustOptimize(TpchQuery(5), Disabling({"order_joins", "pick_build_side"}));
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  EXPECT_THAT(ScanOrder(*plan),
              ElementsAre("customer", "orders", "lineitem", "supplier", "nation", "region"));
  EXPECT_EQ(KeyPairs(*plan).size(), 6u);
}

TEST(JoinOrderTest, FromOrderDefersARelationThatIsNotYetConnected) {
  // lineitem only connects to orders, which comes after it: FROM order puts it last.
  const LogicalPlan plan = MustOptimize(
      "SELECT c_name FROM customer, lineitem, orders "
      "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey",
      Disabling({"order_joins", "pick_build_side"}));
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  EXPECT_THAT(ScanOrder(*plan), ElementsAre("customer", "orders", "lineitem"));
}

TEST(JoinOrderTest, ACommaJoinWithoutAPredicateIsAPlannerError) {
  auto plan = Optimize(MustBind("SELECT n_name, r_name FROM nation, region"), PlannerOptions{});
  ASSERT_FALSE(plan.ok());
  EXPECT_TRUE(plan.status().IsInvalid());
  EXPECT_THAT(plan.status().message(), ::testing::StartsWith("planner error at "));
  EXPECT_THAT(plan.status().message(),
              HasSubstr("requires a cross product between nation and region"));
}

TEST(JoinOrderTest, ThePlannerErrorCarriesThePositionOfTheUnreachedRelation) {
  // `region` is on line 3, column 6: that is where the error points.
  auto plan = Optimize(MustBind("SELECT n_name\nFROM nation,\n     region\nWHERE n_nationkey > 1"),
                       PlannerOptions{});
  ASSERT_FALSE(plan.ok());
  EXPECT_THAT(plan.status().message(), ::testing::StartsWith("planner error at 3:6: "));
  EXPECT_THAT(plan.status().message(), HasSubstr("between nation and region"));
}

TEST(JoinOrderTest, ACrossProductIsRefusedWhateverTheRuleSet) {
  for (const bool order : {true, false}) {
    auto plan = Optimize(MustBind("SELECT n_name, r_name FROM nation, region"),
                         order ? PlannerOptions{} : Disabling({"order_joins"}));
    ASSERT_FALSE(plan.ok());
    EXPECT_THAT(plan.status().message(), HasSubstr("between nation and region"));
  }
}

TEST(JoinOrderTest, TheErrorNamesTheFirstRelationThatCannotBeReached) {
  auto plan = Optimize(
      MustBind("SELECT n_name FROM nation, supplier, region WHERE n_nationkey = s_nationkey"),
      PlannerOptions{});
  ASSERT_FALSE(plan.ok());
  EXPECT_THAT(plan.status().message(), HasSubstr("between nation and region"));
}

TEST(JoinOrderTest, AnInequalityDoesNotConnectRelations) {
  auto plan =
      Optimize(MustBind("SELECT n_name FROM nation, region WHERE n_regionkey < r_regionkey"),
               PlannerOptions{});
  ASSERT_FALSE(plan.ok());
  EXPECT_THAT(plan.status().message(), HasSubstr("requires a cross product"));
}

TEST(JoinOrderTest, TwoKeyPairsBetweenTheSameRelationsFormOneJoin) {
  const LogicalPlan plan = MustOptimize(
      "SELECT l_orderkey FROM lineitem, partsupp "
      "WHERE l_partkey = ps_partkey AND l_suppkey = ps_suppkey",
      PlannerOptions{});
  ASSERT_NE(plan, nullptr);
  ExpectWellFormed(*plan);
  const auto joins = NodesOfKind(*plan, LogicalKind::kJoin);
  ASSERT_EQ(joins.size(), 1u);
  EXPECT_EQ(static_cast<const LogicalJoin&>(*joins[0]).left_keys.size(), 2u);
}

TEST(JoinOrderTest, TheJoinBuildsFromTheSmallerInput) {
  const LogicalPlan plan = MustOptimize(
      "SELECT n_name FROM nation, region WHERE n_regionkey = r_regionkey", PlannerOptions{});
  ASSERT_NE(plan, nullptr);
  // region (5 rows) is the build side: the right input.
  EXPECT_THAT(ScanOrder(*plan), ElementsAre("nation", "region"));
  const LogicalPlan as_written = MustOptimize(
      "SELECT n_name FROM region, nation WHERE n_regionkey = r_regionkey", PlannerOptions{});
  ASSERT_NE(as_written, nullptr);
  EXPECT_THAT(ScanOrder(*as_written), ElementsAre("nation", "region"));
}

TEST(JoinOrderTest, WithPickBuildSideDisabledTheRightInputAsWrittenBuilds) {
  const LogicalPlan plan =
      MustOptimize("SELECT n_name FROM region, nation WHERE n_regionkey = r_regionkey",
                   Disabling({"order_joins", "pick_build_side"}));
  ASSERT_NE(plan, nullptr);
  EXPECT_THAT(ScanOrder(*plan), ElementsAre("region", "nation"));
}

}  // namespace
}  // namespace quarry
