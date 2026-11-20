// Aggregations whose groups do not fit in the memory budget spill their partial states and merge
// them partition by partition; the answer must be the one an unlimited run gives.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "quarry/engine.h"
#include "support/arrays.h"
#include "support/parquet_fixture.h"

namespace quarry {
namespace {

namespace fs = std::filesystem;
using testing::Cell;
using testing::TempDir;

constexpr int64_t kLimit = 64 * 1024;
constexpr int64_t kMargin = 256 * 1024;

std::vector<std::string> SortedRows(const arrow::Table& table) {
  std::vector<std::string> rows(static_cast<size_t>(table.num_rows()));
  for (int c = 0; c < table.num_columns(); ++c) {
    int64_t row = 0;
    for (const auto& chunk : table.column(c)->chunks()) {
      for (int64_t i = 0; i < chunk->length(); ++i, ++row) {
        rows[static_cast<size_t>(row)] += Cell(*chunk, i) + "|";
      }
    }
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

class HighCardinalityAggTest : public ::testing::Test {
 protected:
  std::unique_ptr<Engine> MakeEngine(int64_t limit, int32_t batch_size = 2048) {
    EngineOptions options;
    options.memory_limit_bytes = limit;
    options.batch_size = batch_size;
    options.spill_parent = parent_.path();
    auto engine = std::make_unique<Engine>(std::move(options));
    const arrow::Status st = engine->RegisterDirectory("tests/data/tpch-sf0.01");
    EXPECT_TRUE(st.ok()) << st.ToString();
    return engine;
  }

  // Runs `sql` unlimited and at 64 KiB: the same rows, spilled at 64 KiB.
  void ExpectSameRowsWhenSpilling(const std::string& sql, int32_t batch_size = 2048) {
    auto reference = MakeEngine(0, batch_size)->Execute(sql);
    ASSERT_TRUE(reference.ok()) << reference.status().ToString();
    auto spilled = MakeEngine(kLimit, batch_size)->Execute(sql);
    ASSERT_TRUE(spilled.ok()) << spilled.status().ToString();
    EXPECT_EQ(SortedRows(*spilled->table), SortedRows(*reference->table));
    int64_t bytes = 0;
    for (const OperatorProfile& p : spilled->profile) {
      bytes += p.spilled_bytes;
      EXPECT_LE(p.peak_state_bytes, kLimit) << p.name << " #" << p.id;
    }
    EXPECT_LE(spilled->pool_peak_bytes, kLimit + kMargin) << "allocated beyond budget and margin";
    EXPECT_GT(bytes, 0) << "the query was meant to spill";
    EXPECT_TRUE(fs::is_empty(parent_.path()));
  }

  TempDir parent_;
};

TEST_F(HighCardinalityAggTest, SumPerOrderKeyAtSixtyFourKiBSpillsAndEqualsTheUnlimitedResult) {
  ExpectSameRowsWhenSpilling(
      "SELECT l_orderkey, SUM(l_extendedprice) FROM lineitem GROUP BY l_orderkey");
}

TEST_F(HighCardinalityAggTest, EveryAggregateFunctionMergesItsPartialStates) {
  ExpectSameRowsWhenSpilling(
      "SELECT l_orderkey, COUNT(*), COUNT(l_comment), SUM(l_quantity), AVG(l_discount), "
      "MIN(l_shipdate), MAX(l_shipdate), MIN(l_comment), MAX(l_comment), MIN(l_extendedprice), "
      "MAX(l_linenumber), SUM(l_linenumber), AVG(l_linenumber) "
      "FROM lineitem GROUP BY l_orderkey");
}

TEST_F(HighCardinalityAggTest, AStringKeyAndSeveralKeysSpillToo) {
  ExpectSameRowsWhenSpilling(
      "SELECT l_comment, COUNT(*), SUM(l_quantity) FROM lineitem GROUP BY l_comment");
  ExpectSameRowsWhenSpilling(
      "SELECT l_orderkey, l_suppkey, MAX(l_shipdate) FROM lineitem GROUP BY l_orderkey, l_suppkey");
}

TEST_F(HighCardinalityAggTest, SmallBatchesGiveTheSameAnswer) {
  ExpectSameRowsWhenSpilling(
      "SELECT l_orderkey, COUNT(*), SUM(l_extendedprice) FROM lineitem GROUP BY l_orderkey", 7);
}

TEST_F(HighCardinalityAggTest, AnAggregateWithoutGroupByNeverNeedsToSpill) {
  auto result = MakeEngine(kLimit)->Execute(
      "SELECT COUNT(*), SUM(l_extendedprice), MIN(l_comment), AVG(l_quantity) FROM lineitem");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  auto reference = MakeEngine(0)->Execute(
      "SELECT COUNT(*), SUM(l_extendedprice), MIN(l_comment), AVG(l_quantity) FROM lineitem");
  ASSERT_TRUE(reference.ok());
  EXPECT_EQ(SortedRows(*result->table), SortedRows(*reference->table));
  for (const OperatorProfile& p : result->profile) EXPECT_EQ(p.spilled_bytes, 0) << p.name;
}

TEST_F(HighCardinalityAggTest, NullKeysFormOneGroupAfterSpilling) {
  // 30000 rows: key NULL for every third row, others spread over 12000 keys; the value column
  // carries NULLs too.
  TempDir data;
  fs::create_directories(data.path() / "t");
  std::vector<int64_t> keys;
  std::vector<int64_t> values;
  testing::NullSet key_nulls;
  testing::NullSet value_nulls;
  for (int64_t i = 0; i < 30000; ++i) {
    keys.push_back(i % 12000);
    values.push_back(i);
    if (i % 3 == 0) key_nulls.insert(static_cast<size_t>(i));
    if (i % 7 == 0) value_nulls.insert(static_cast<size_t>(i));
  }
  const fs::path file = testing::WriteParquet(
      data.path() / "t" / "n.parquet",
      {{arrow::field("k", arrow::int64()), testing::Int64Array(keys, key_nulls)},
       {arrow::field("v", arrow::int64()), testing::Int64Array(values, value_nulls)}});
  const std::string sql = "SELECT k, COUNT(*), COUNT(v), SUM(v), MIN(v), MAX(v) FROM n GROUP BY k";
  std::vector<std::vector<std::string>> results;
  for (const int64_t limit : {int64_t{0}, kLimit}) {
    EngineOptions options;
    options.memory_limit_bytes = limit;
    options.spill_parent = parent_.path();
    Engine engine(options);
    ASSERT_TRUE(engine.RegisterTable("n", {file}).ok());
    auto result = engine.Execute(sql);
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    results.push_back(SortedRows(*result->table));
  }
  EXPECT_EQ(results[0], results[1]);
  EXPECT_EQ(results[0].size(), 8001u);  // 8000 distinct non-NULL keys + the NULL group
  EXPECT_TRUE(fs::is_empty(parent_.path()));
}

}  // namespace
}  // namespace quarry
