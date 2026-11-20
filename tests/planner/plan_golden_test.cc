// Plans of the TPC-H queries against the SF0.01 catalog, as Explain prints them, compared with
// tests/planner/plans/*.plan. UPDATE_GOLDENS=1 rewrites the files; review the diff before
// committing.

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "plan/explain.h"
#include "planner/physical_planner.h"
#include "support/planner_helpers.h"

namespace quarry {
namespace {

using testing::Disabling;
using testing::MustBind;
using testing::ReadTextFile;
using testing::TpchQuery;

std::string PlanText(int query, const PlannerOptions& options) {
  LogicalPlan logical = MustBind(TpchQuery(query));
  if (logical == nullptr) return "";
  auto optimized = Optimize(std::move(logical), options);
  EXPECT_TRUE(optimized.ok()) << optimized.status().ToString();
  if (!optimized.ok()) return "";
  auto physical = PlanPhysical(*optimized, options);
  EXPECT_TRUE(physical.ok()) << physical.status().ToString();
  if (!physical.ok()) return "";
  return Explain(**physical);
}

void CheckGolden(const std::string& name, const std::string& actual) {
  const std::string path = "tests/planner/plans/" + name + ".plan";
  const char* update = std::getenv("UPDATE_GOLDENS");
  if (update != nullptr && std::string(update) == "1") {
    std::ofstream out(path, std::ios::trunc);
    ASSERT_TRUE(out.good()) << path;
    out << actual;
    return;
  }
  EXPECT_EQ(actual, ReadTextFile(path)) << path;
}

TEST(PlanGoldenTest, Q1) { CheckGolden("q1", PlanText(1, PlannerOptions{})); }
TEST(PlanGoldenTest, Q3) { CheckGolden("q3", PlanText(3, PlannerOptions{})); }
TEST(PlanGoldenTest, Q5) { CheckGolden("q5", PlanText(5, PlannerOptions{})); }
TEST(PlanGoldenTest, Q6) { CheckGolden("q6", PlanText(6, PlannerOptions{})); }

// The same query with the two join rules off: FROM order, build side as written.
TEST(PlanGoldenTest, Q5WithoutJoinOrdering) {
  CheckGolden("q5_from_order", PlanText(5, Disabling({"order_joins", "pick_build_side"})));
}

}  // namespace
}  // namespace quarry
