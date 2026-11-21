// The memory guarantee, measured where it is most likely to fail: a hash table that grows until it
// reaches the limit. The aggregate's groups and the join's build side are far larger than the
// smaller limits (they spill) and about as large as the largest (they nearly fit). At every limit
// the memory the query's operators allocated, as the query's own pool counts it, must stay within
// the limit plus a margin that is a stated function of what is allocated beyond the charge; see
// docs/design/0006-memory-and-spill.md.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "quarry/engine.h"
#include "support/arrays.h"
#include "support/parquet_fixture.h"

namespace quarry {
namespace {

namespace fs = std::filesystem;
using testing::Int64Array;
using testing::TempDir;

constexpr int64_t kMiB = 1024 * 1024;
constexpr int64_t kKiB = 1024;

// What the operators' pool may hold beyond the limit: spill-writer buffers, one input batch and
// expression temporaries. Measured over the cases below, the pool peak never reaches the limit
// (hash-table storage is charged before it is allocated), so the fixed margin is what is left for
// the uncharged parts; ADR 0006 states it and what it does not cover.
constexpr int64_t kMargin = 256 * kKiB;

int64_t Margin(int64_t) { return kMargin; }

class NearLimitTest : public ::testing::Test {
 protected:
  static constexpr int64_t kAggRows = 600000;
  static constexpr int64_t kJoinBuildRows = 500000;
  static constexpr int64_t kJoinProbeRows = 600000;

  void SetUp() override {
    std::vector<int64_t> keys(kAggRows);
    std::vector<int64_t> values(kAggRows);
    for (int64_t i = 0; i < kAggRows; ++i) {
      keys[static_cast<size_t>(i)] = i;
      values[static_cast<size_t>(i)] = 3 * i + 1;
    }
    groups_ = testing::WriteParquet(dir_.path() / "groups.parquet",
                                    {{arrow::field("k", arrow::int64()), Int64Array(keys)},
                                     {arrow::field("v", arrow::int64()), Int64Array(values)}});
    std::vector<int64_t> build_keys(kJoinBuildRows);
    std::vector<int64_t> p1(kJoinBuildRows);
    std::vector<int64_t> p2(kJoinBuildRows);
    std::vector<int64_t> p3(kJoinBuildRows);
    for (int64_t i = 0; i < kJoinBuildRows; ++i) {
      const auto at = static_cast<size_t>(i);
      build_keys[at] = i;
      p1[at] = i + 1;
      p2[at] = 2 * i;
      p3[at] = 7;
    }
    build_ = testing::WriteParquet(dir_.path() / "build.parquet",
                                   {{arrow::field("k", arrow::int64()), Int64Array(build_keys)},
                                    {arrow::field("p1", arrow::int64()), Int64Array(p1)},
                                    {arrow::field("p2", arrow::int64()), Int64Array(p2)},
                                    {arrow::field("p3", arrow::int64()), Int64Array(p3)}});
    std::vector<int64_t> probe_keys(kJoinProbeRows);
    for (int64_t i = 0; i < kJoinProbeRows; ++i) {
      probe_keys[static_cast<size_t>(i)] = i % kJoinBuildRows;
    }
    probe_ = testing::WriteParquet(dir_.path() / "probe.parquet",
                                   {{arrow::field("k", arrow::int64()), Int64Array(probe_keys)}});
  }

  // A build side whose rows are wide: about 6.4 MiB of payload under an 8 MiB limit, with a small
  // hash table. The charge for it nearly reaches the limit while the concatenation of the build
  // rows holds a second and third copy that the charge does not cover.
  void WriteWide() {
    constexpr int64_t kRows = 16000;
    std::vector<int64_t> keys(kRows);
    std::vector<std::string> text(kRows);
    for (int64_t i = 0; i < kRows; ++i) {
      keys[static_cast<size_t>(i)] = i;
      text[static_cast<size_t>(i)] = std::string(400, static_cast<char>('a' + i % 26));
    }
    wide_ = testing::WriteParquet(dir_.path() / "wide.parquet",
                                  {{arrow::field("k", arrow::int64()), Int64Array(keys)},
                                   {arrow::field("t", arrow::utf8()), testing::StringArray(text)}});
    std::vector<int64_t> probe(kRows * 4);
    for (size_t i = 0; i < probe.size(); ++i) probe[i] = static_cast<int64_t>(i) % kRows;
    wide_probe_ = testing::WriteParquet(dir_.path() / "wide_probe.parquet",
                                        {{arrow::field("k", arrow::int64()), Int64Array(probe)}});
  }

  QueryResult Run(const std::string& sql, int64_t limit) {
    EngineOptions options;
    options.memory_limit_bytes = limit;
    options.spill_parent = spill_.path();
    Engine engine(options);
    EXPECT_TRUE(engine.RegisterTable("groups", {groups_}).ok());
    EXPECT_TRUE(engine.RegisterTable("build", {build_}).ok());
    EXPECT_TRUE(engine.RegisterTable("probe", {probe_}).ok());
    if (!wide_.empty()) {
      EXPECT_TRUE(engine.RegisterTable("wide", {wide_}).ok());
      EXPECT_TRUE(engine.RegisterTable("wide_probe", {wide_probe_}).ok());
    }
    auto result = engine.Execute(sql);
    EXPECT_TRUE(result.ok()) << sql << " at " << limit << ": " << result.status().ToString();
    if (!result.ok()) return {};
    EXPECT_TRUE(fs::is_empty(spill_.path())) << "spill files were left behind";
    return std::move(*result);
  }

  static int64_t Spilled(const QueryResult& result) {
    int64_t bytes = 0;
    for (const OperatorProfile& p : result.profile) bytes += p.spilled_bytes;
    return bytes;
  }

  static std::string Cell0(const QueryResult& result, int column) {
    return testing::Cell(*result.table->column(column)->chunk(0), 0);
  }

  // The sum of an integer column (BIGINT or DECIMAL(38, 0)) over every row, as text: the answer
  // must not depend on the limit.
  static std::string ColumnSum(const QueryResult& result, int column) {
    int64_t sum = 0;
    for (const auto& chunk : result.table->column(column)->chunks()) {
      for (int64_t i = 0; i < chunk->length(); ++i) sum += std::stoll(testing::Cell(*chunk, i));
    }
    return std::to_string(result.table->num_rows()) + ":" + std::to_string(sum);
  }

  void Report(const char* what, int64_t limit, const QueryResult& result) {
    std::printf(
        "NEAR-LIMIT %-9s limit %3lld MiB: pool peak %8.2f MiB, over limit %8.2f MiB, scan pool "
        "%6.2f MiB, spilled %s\n",
        what, static_cast<long long>(limit / kMiB),
        static_cast<double>(result.pool_peak_bytes) / static_cast<double>(kMiB),
        static_cast<double>(result.pool_peak_bytes - limit) / static_cast<double>(kMiB),
        static_cast<double>(result.scan_pool_peak_bytes) / static_cast<double>(kMiB),
        Spilled(result) > 0 ? "yes" : "no");
  }

  TempDir dir_;
  TempDir spill_;
  fs::path groups_;
  fs::path build_;
  fs::path probe_;
  fs::path wide_;
  fs::path wide_probe_;
};

TEST_F(NearLimitTest, AHighCardinalityAggregateStaysWithinTheLimitAndTheMargin) {
  const std::string sql = "SELECT k, SUM(v), COUNT(*), AVG(v), MIN(v) FROM groups GROUP BY k";
  const QueryResult reference = Run(sql, 0);
  EXPECT_GT(reference.pool_peak_bytes, 16 * kMiB) << "the aggregate state is not in the pool";
  Report("aggregate", 0, reference);
  for (const int64_t limit : {1 * kMiB, 16 * kMiB, 64 * kMiB, 128 * kMiB}) {
    const QueryResult result = Run(sql, limit);
    Report("aggregate", limit, result);
    for (const int column : {0, 1, 2, 4}) {
      EXPECT_EQ(ColumnSum(result, column), ColumnSum(reference, column)) << "column " << column;
    }
    if (limit <= 16 * kMiB) {
      EXPECT_GT(Spilled(result), 0) << "the limit was meant to force a spill";
    }
    EXPECT_LE(result.pool_peak_bytes, limit + Margin(limit)) << "limit " << limit;
  }
}

TEST_F(NearLimitTest, ALargeJoinStaysWithinTheLimitAndTheMargin) {
  const std::string sql =
      "SELECT COUNT(*), SUM(b.p1), SUM(b.p2) FROM probe p JOIN build b ON p.k = b.k";
  const QueryResult reference = Run(sql, 0);
  Report("join", 0, reference);
  for (const int64_t limit : {1 * kMiB, 16 * kMiB, 64 * kMiB}) {
    const QueryResult result = Run(sql, limit);
    Report("join", limit, result);
    EXPECT_EQ(Cell0(result, 0), Cell0(reference, 0));
    EXPECT_EQ(Cell0(result, 1), Cell0(reference, 1));
    EXPECT_EQ(Cell0(result, 2), Cell0(reference, 2));
    if (limit <= 16 * kMiB) {
      EXPECT_GT(Spilled(result), 0) << "the limit was meant to force a spill";
    }
    EXPECT_LE(result.pool_peak_bytes, limit + Margin(limit)) << "limit " << limit;
  }
}

TEST_F(NearLimitTest, AJoinWhoseBuildRowsNearlyFillTheLimitStaysWithinTheMargin) {
  WriteWide();
  const std::string sql =
      "SELECT COUNT(*), MIN(w.t), SUM(w.k) FROM wide_probe p JOIN wide w ON p.k = w.k";
  const QueryResult reference = Run(sql, 0);
  Report("wide join", 0, reference);
  for (const int64_t limit : {2 * kMiB, 8 * kMiB, 16 * kMiB}) {
    const QueryResult result = Run(sql, limit);
    Report("wide join", limit, result);
    EXPECT_EQ(Cell0(result, 0), Cell0(reference, 0));
    EXPECT_EQ(Cell0(result, 1), Cell0(reference, 1));
    EXPECT_LE(result.pool_peak_bytes, limit + Margin(limit)) << "limit " << limit;
  }
}

}  // namespace
}  // namespace quarry
