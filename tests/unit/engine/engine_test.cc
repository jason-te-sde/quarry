#include "quarry/engine.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "support/arrays.h"
#include "support/parquet_fixture.h"

namespace quarry {
namespace {

using testing::Cell;
using testing::Int64Column;
using testing::StringColumn;
using testing::TempDir;
using testing::WriteParquet;
using testing::WriteParquetWithoutRowGroups;
using Clock = std::chrono::steady_clock;

constexpr const char* kTpch = "tests/data/tpch-sf0.01";

std::unique_ptr<Engine> TpchEngine(EngineOptions options = {}) {
  auto engine = std::make_unique<Engine>(std::move(options));
  const arrow::Status st = engine->RegisterDirectory(kTpch);
  EXPECT_TRUE(st.ok()) << st.ToString();
  return engine;
}

std::vector<std::string> Row(const arrow::Table& table, int64_t row) {
  std::vector<std::string> cells;
  for (int c = 0; c < table.num_columns(); ++c) {
    cells.push_back(Cell(*table.column(c)->chunk(0), row));
  }
  return cells;
}

TEST(EngineTest, ExecuteReturnsATableWithNamesTypesAndRows) {
  auto engine = TpchEngine();
  auto result =
      engine->Execute("SELECT n_name, n_nationkey AS key FROM nation ORDER BY n_nationkey LIMIT 3");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  const arrow::Table& table = *result->table;
  EXPECT_EQ(table.schema()->field(0)->name(), "n_name");
  EXPECT_EQ(table.schema()->field(1)->name(), "key");
  EXPECT_TRUE(table.schema()->field(0)->type()->Equals(arrow::utf8()));
  EXPECT_TRUE(table.schema()->field(1)->type()->Equals(arrow::int32()));
  ASSERT_EQ(table.num_rows(), 3);
  EXPECT_EQ(Row(table, 0), (std::vector<std::string>{"ALGERIA", "0"}));
  EXPECT_EQ(Row(table, 2), (std::vector<std::string>{"BRAZIL", "2"}));
}

TEST(EngineTest, OutputColumnNamesKeepTheCaseTheUserTyped) {
  auto engine = TpchEngine();
  auto result = engine->Execute("SELECT N_NAME, n_NationKey FROM Nation LIMIT 1");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(result->table->schema()->field(0)->name(), "N_NAME");
  EXPECT_EQ(result->table->schema()->field(1)->name(), "n_NationKey");
}

TEST(EngineTest, TheProfileCoversEveryOperatorOfTheQuery) {
  auto engine = TpchEngine();
  auto result = engine->Execute("SELECT r_name FROM region WHERE r_regionkey < 3");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  ASSERT_FALSE(result->profile.empty());
  EXPECT_EQ(result->profile[0].id, 0);
  EXPECT_EQ(result->profile[0].rows_out, result->table->num_rows());
  EXPECT_EQ(result->profile.back().name, "Scan");
  EXPECT_GT(result->profile.back().scan_buffer_bytes, 0);
}

TEST(EngineTest, ExplainWithoutAnalyzeIsThePlanWithEstimates) {
  auto engine = TpchEngine();
  auto text = engine->Explain("SELECT r_name FROM region WHERE r_regionkey < 3", false);
  ASSERT_TRUE(text.ok()) << text.status().ToString();
  EXPECT_NE(text->find("Scan #"), std::string::npos) << *text;
  EXPECT_NE(text->find("est="), std::string::npos);
  EXPECT_EQ(text->find("rows="), std::string::npos) << "nothing has run";
}

TEST(EngineTest, ExplainAnalyzeListsEveryOperatorWithRowsBatchesTimeAndPeak) {
  auto engine = TpchEngine();
  const std::string sql =
      "SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) AS revenue "
      "FROM customer, orders, lineitem, supplier, nation, region "
      "WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey "
      "AND c_nationkey = s_nationkey AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey "
      "AND r_name = 'ASIA' AND o_orderdate >= DATE '1994-01-01' "
      "AND o_orderdate < DATE '1995-01-01' GROUP BY n_name ORDER BY revenue DESC";
  auto plan = engine->Explain(sql, false);
  auto analyzed = engine->Explain(sql, true);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_TRUE(analyzed.ok()) << analyzed.status().ToString();
  const auto lines = [](const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
      const size_t end = text.find('\n', start);
      out.push_back(text.substr(start, end - start));
      start = end + 1;
    }
    return out;
  };
  const auto plan_lines = lines(*plan);
  auto analyzed_lines = lines(*analyzed);
  // The footer: what the query's memory pools peaked at, whether or not the budget charged it.
  ASSERT_FALSE(analyzed_lines.empty());
  EXPECT_EQ(analyzed_lines.back().rfind("pool_peak=", 0), 0u) << analyzed_lines.back();
  EXPECT_NE(analyzed_lines.back().find(" scan_pool_peak="), std::string::npos);
  analyzed_lines.pop_back();
  ASSERT_EQ(plan_lines.size(), analyzed_lines.size());
  ASSERT_GE(plan_lines.size(), 12u);  // 6 scans, 5 joins, aggregate, sort, projections
  for (size_t i = 0; i < plan_lines.size(); ++i) {
    SCOPED_TRACE(analyzed_lines[i]);
    EXPECT_EQ(analyzed_lines[i].rfind(plan_lines[i], 0), 0u) << "the plan line comes first";
    for (const char* field : {" rows=", " batches=", " time=", " peak="}) {
      EXPECT_NE(analyzed_lines[i].find(field), std::string::npos) << field;
    }
  }
  std::printf("%s", analyzed->c_str());  // the EXPLAIN ANALYZE of this query, for the record
}

TEST(EngineTest, ErrorsCarryTheirKindAndPosition) {
  auto engine = TpchEngine();
  const auto message = [&](const std::string& sql) {
    auto result = engine->Execute(sql);
    EXPECT_FALSE(result.ok());
    return result.ok() ? std::string() : result.status().message();
  };
  EXPECT_EQ(message("SELEC 1").rfind("syntax error at 1:1: ", 0), 0u);
  EXPECT_EQ(message("SELECT nope FROM nation").rfind("binder error at 1:8: ", 0), 0u);
  EXPECT_EQ(message("SELECT n_name\nFROM nation,\n     region").rfind("planner error at 3:6: ", 0),
            0u);
}

TEST(EngineTest, OptionsAreValidated) {
  EngineOptions bad_batch;
  bad_batch.batch_size = 0;
  auto engine = TpchEngine(bad_batch);
  EXPECT_FALSE(engine->Execute("SELECT 1 FROM nation").ok());

  EngineOptions bad_rule;
  bad_rule.disabled_rules = {"no_such_rule"};
  auto second = TpchEngine(bad_rule);
  auto result = second->Execute("SELECT n_name FROM nation");
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.status().message().find("no_such_rule"), std::string::npos);
}

TEST(EngineTest, EmptyInputsFollowSqlSemantics) {
  TempDir dir;
  const auto file =
      WriteParquetWithoutRowGroups(dir.path() / "empty.parquet", {Int64Column("x", {})});
  Engine engine{EngineOptions{}};
  ASSERT_TRUE(engine.RegisterTable("empty", {file}).ok());

  auto plain = engine.Execute("SELECT x FROM empty");
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  EXPECT_EQ(plain->table->num_rows(), 0);

  auto global =
      engine.Execute("SELECT COUNT(*), COUNT(x), SUM(x), MIN(x), MAX(x), AVG(x) FROM empty");
  ASSERT_TRUE(global.ok()) << global.status().ToString();
  ASSERT_EQ(global->table->num_rows(), 1);
  EXPECT_EQ(Row(*global->table, 0),
            (std::vector<std::string>{"0", "0", "NULL", "NULL", "NULL", "NULL"}));

  auto grouped = engine.Execute("SELECT x, COUNT(*) FROM empty GROUP BY x");
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  EXPECT_EQ(grouped->table->num_rows(), 0);
  EXPECT_EQ(grouped->table->num_columns(), 2);

  // The same through a filter that removes every row of a real table.
  auto filtered = engine.RegisterDirectory(kTpch);
  ASSERT_TRUE(filtered.ok());
  auto none = engine.Execute("SELECT COUNT(*), SUM(n_nationkey) FROM nation WHERE n_nationkey < 0");
  ASSERT_TRUE(none.ok()) << none.status().ToString();
  EXPECT_EQ(Row(*none->table, 0), (std::vector<std::string>{"0", "NULL"}));
}

TEST(EngineTest, AnEngineCanRunQueryAfterQuery) {
  auto engine = TpchEngine();
  for (int i = 0; i < 3; ++i) {
    auto result = engine->Execute("SELECT COUNT(*) FROM lineitem");
    ASSERT_TRUE(result.ok()) << result.status().ToString();
    EXPECT_EQ(Row(*result->table, 0), (std::vector<std::string>{"60175"}));
  }
}

// A table whose rows all share one key: joining it with itself yields n * n rows, so the query
// runs for as long as n needs and ends only when it is cancelled.
class LongQueryTest : public ::testing::Test {
 protected:
  static constexpr const char* kLongQuery = "SELECT COUNT(*) FROM big a JOIN big b ON a.k = b.k";

  void SetUp() override {
    std::vector<int64_t> keys(30000, 1);
    std::vector<int64_t> ids;
    for (int64_t i = 0; i < 30000; ++i) ids.push_back(i);
    file_ =
        WriteParquet(dir_.path() / "big.parquet", {Int64Column("k", keys), Int64Column("id", ids)});
  }

  std::unique_ptr<Engine> MakeEngine(EngineOptions options = {}) {
    auto engine = std::make_unique<Engine>(std::move(options));
    EXPECT_TRUE(engine->RegisterTable("big", {file_}).ok());
    return engine;
  }

  TempDir dir_;
  std::filesystem::path file_;
};

TEST_F(LongQueryTest, ASecondExecuteWhileOneRunsIsRefused) {
  auto engine = MakeEngine();
  std::atomic<bool> started{false};
  arrow::Result<QueryResult> long_result = arrow::Status::Invalid("not run");
  std::thread runner([&] {
    started = true;
    long_result = engine->Execute(kLongQuery);
  });
  while (!started) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto second = engine->Execute("SELECT 1 FROM big");
  ASSERT_FALSE(second.ok());
  EXPECT_TRUE(second.status().IsInvalid());
  EXPECT_NE(second.status().message().find("engine is busy"), std::string::npos)
      << second.status().message();
  auto explain = engine->Explain("SELECT 1 FROM big", false);
  ASSERT_FALSE(explain.ok());
  EXPECT_NE(explain.status().message().find("engine is busy"), std::string::npos);

  engine->Cancel();
  runner.join();
  EXPECT_TRUE(long_result.status().IsCancelled()) << long_result.status().ToString();
  // The engine is usable again.
  EXPECT_TRUE(engine->Execute("SELECT COUNT(*) FROM big").ok());
}

TEST_F(LongQueryTest, CancelEndsTheQueryWithinAHundredMilliseconds) {
  auto engine = MakeEngine();
  arrow::Result<QueryResult> result = arrow::Status::Invalid("not run");
  Clock::time_point finished;
  std::thread runner([&] {
    result = engine->Execute(kLongQuery);
    finished = Clock::now();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const Clock::time_point cancelled_at = Clock::now();
  engine->Cancel();
  runner.join();
  ASSERT_FALSE(result.ok()) << "the query was meant to outlast the test";
  EXPECT_TRUE(result.status().IsCancelled()) << result.status().ToString();
  const auto latency =
      std::chrono::duration_cast<std::chrono::milliseconds>(finished - cancelled_at);
  EXPECT_LT(latency.count(), 100) << "cancel took " << latency.count() << " ms";
}

TEST_F(LongQueryTest, ACancelBeforeAQueryStartsDoesNotAffectIt) {
  auto engine = MakeEngine();
  engine->Cancel();
  EXPECT_TRUE(engine->Execute("SELECT COUNT(*) FROM big").ok());
}

TEST_F(LongQueryTest, ATimeoutEndsTheQueryAsCancelledAndNamesTheTimeout) {
  EngineOptions options;
  options.timeout = std::chrono::milliseconds(1);
  auto engine = MakeEngine(options);
  const Clock::time_point start = Clock::now();
  auto result = engine->Execute(kLongQuery);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(result.status().IsCancelled()) << result.status().ToString();
  EXPECT_NE(result.status().message().find("timeout of 1 ms"), std::string::npos)
      << result.status().message();
  EXPECT_LT(elapsed.count(), 1000);
}

// Batch size 1 turns a build side into one tiny batch per row. The timeout must still end the query
// as Cancelled, within the timeout plus a margin that does not grow with the build side.
TEST(LargeBuildTimeoutTest, ATimeoutOnALargeBuildSideAtBatchSizeOneEndsAsCancelled) {
  TempDir dir;
  std::vector<int64_t> ids;
  for (int64_t i = 0; i < 3000000; ++i) ids.push_back(i);
  const auto file = WriteParquet(dir.path() / "wide.parquet", {Int64Column("id", ids)});
  EngineOptions options;
  options.batch_size = 1;
  options.timeout = std::chrono::milliseconds(300);
  Engine engine(options);
  ASSERT_TRUE(engine.RegisterTable("wide", {file}).ok());
  const Clock::time_point start = Clock::now();
  auto result = engine.Execute("SELECT COUNT(*) FROM wide a JOIN wide b ON a.id = b.id");
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  ASSERT_FALSE(result.ok()) << "the query was meant to outlast its timeout";
  EXPECT_TRUE(result.status().IsCancelled()) << result.status().ToString();
  EXPECT_LT(elapsed.count(), 300 + 5000) << "cancelled after " << elapsed.count() << " ms";
}

}  // namespace
}  // namespace quarry
