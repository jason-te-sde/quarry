// TPC-H Q1, Q3, Q5 and Q6 under a memory budget: the answer must be DuckDB's whatever the budget,
// the joins and aggregations that do not fit must spill (Q3 and Q5 at 64 KiB), the ones that do fit
// must not, and the memory the operators allocate (the query's pool peak, which counts what the
// budget does not charge: spill buffers, the batch in flight, concatenation copies) must stay
// within the budget plus the margin recorded in docs/design/0006-memory-and-spill.md. Scans
// allocate from a pool of their own, reported separately and not included.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "output_format.h"
#include "quarry/engine.h"
#include "support/parquet_fixture.h"

namespace quarry {
namespace {

namespace fs = std::filesystem;

// What operators may allocate beyond the budget: partition buffers, one input batch and copies,
// which are not charged.
constexpr int64_t kMargin = 256 * 1024;

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string ColumnsAndRows(std::string json) {
  while (!json.empty() && (json.back() == '\n' || json.back() == '\r')) json.pop_back();
  const size_t version = json.find(",\"duckdb_version\"");
  if (version != std::string::npos) json = json.substr(0, version) + "}";
  return json;
}

struct Case {
  int query;
  int64_t budget;  // 0 = unlimited
};

void PrintTo(const Case& c, std::ostream* os) { *os << "q" << c.query << " budget " << c.budget; }

class TpchBudgetTest : public ::testing::TestWithParam<Case> {};

TEST_P(TpchBudgetTest, ResultMatchesDuckDBAndStaysWithinTheBudget) {
  const Case& c = GetParam();
  testing::TempDir spill_parent;
  EngineOptions options;
  options.memory_limit_bytes = c.budget;
  options.spill_parent = spill_parent.path();
  Engine engine(options);
  ASSERT_TRUE(engine.RegisterDirectory("tests/data/tpch-sf0.01").ok());
  auto result = engine.Execute(ReadFile("tests/tpch/queries/q" + std::to_string(c.query) + ".sql"));
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(ToJson(*result->table),
            ColumnsAndRows(ReadFile("tests/tpch/answers/q" + std::to_string(c.query) + ".json")));

  int64_t spilled = 0;
  for (const OperatorProfile& p : result->profile) {
    spilled += p.spilled_bytes;
    if (c.budget > 0) {
      EXPECT_LE(p.peak_state_bytes, c.budget) << p.name << " #" << p.id;  // the charged state
    }
  }
  EXPECT_GT(result->pool_peak_bytes, 0) << "the query pool measured nothing";
  if (c.budget > 0) {
    EXPECT_LE(result->pool_peak_bytes, c.budget + kMargin)
        << "operators allocated far more than the budget they were charged";
  }
  const bool must_spill = c.budget == 64 * 1024 && (c.query == 3 || c.query == 5);
  const bool must_not_spill = c.budget == 0 || (c.budget == 64 * 1024 && !must_spill);
  if (must_spill) {
    EXPECT_GT(spilled, 0) << "the budget was meant to force a spill";
  }
  if (must_not_spill) {
    EXPECT_EQ(spilled, 0) << "nothing here needs to spill";
  }
  EXPECT_TRUE(fs::is_empty(spill_parent.path())) << "spill files were left behind";
}

std::vector<Case> AllCases() {
  std::vector<Case> cases;
  for (const int query : {1, 3, 5, 6}) {
    for (const int64_t budget : {int64_t{64} * 1024, int64_t{1024} * 1024, int64_t{0}}) {
      cases.push_back({query, budget});
    }
  }
  return cases;
}

std::string CaseName(const ::testing::TestParamInfo<Case>& param) {
  return "q" + std::to_string(param.param.query) + "_" +
         (param.param.budget == 0 ? std::string("unlimited")
                                  : std::to_string(param.param.budget / 1024) + "KiB");
}

INSTANTIATE_TEST_SUITE_P(Budgets, TpchBudgetTest, ::testing::ValuesIn(AllCases()), CaseName);

}  // namespace
}  // namespace quarry
