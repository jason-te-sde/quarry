#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "io/spill_directory.h"
#include "io/spill_file.h"
#include "quarry/engine.h"
#include "support/arrays.h"
#include "support/parquet_fixture.h"
#include "support/vector_source.h"

namespace quarry {
namespace {

namespace fs = std::filesystem;
using testing::ArrayPtr;
using testing::BatchOf;
using testing::Cell;
using testing::TempDir;
using Clock = std::chrono::steady_clock;

std::shared_ptr<arrow::Schema> AllTypesSchema() {
  return arrow::schema(
      {arrow::field("b", arrow::boolean()), arrow::field("i", arrow::int32()),
       arrow::field("l", arrow::int64()), arrow::field("d", arrow::decimal64(10, 2)),
       arrow::field("w", arrow::decimal128(30, 4)), arrow::field("f", arrow::float64()),
       arrow::field("t", arrow::date32()), arrow::field("s", arrow::utf8())});
}

Batch AllTypesBatch(int base) {
  const std::vector<int32_t> ints = {base, base + 1, base + 2, base + 3};
  return BatchOf({testing::BoolArray({true, false, true, false}, {1}),
                  testing::Int32Array(ints, {0}),
                  testing::Int64Array({int64_t{1} << 40, -5, 0, base}, {2}),
                  testing::Decimal64Array({12345, -1, 0, 99}, 10, 2, {3}),
                  testing::Decimal128Array({Int128{1} << 70, -3, 7, 0}, 30, 4, {0}),
                  testing::DoubleArray({1.5, -0.0, 3.25e100, 0.0}, {1}),
                  testing::DateArray({0, 19000, -3, 20000}, {2}),
                  testing::StringArray({"a", "", "hello world", "x"}, {1})});
}

std::vector<std::string> RowTexts(const Batch& batch) {
  std::vector<std::string> rows;
  for (int64_t r = 0; r < batch.length; ++r) {
    std::string row;
    for (const auto& column : batch.columns) row += Cell(*column, r) + "|";
    rows.push_back(row);
  }
  return rows;
}

std::vector<Batch> ReadAll(const fs::path& path, SpillFault fault = {}) {
  auto reader = SpillReader::Open(path, std::move(fault));
  EXPECT_TRUE(reader.ok()) << reader.status().ToString();
  std::vector<Batch> out;
  if (!reader.ok()) return out;
  for (;;) {
    auto next = (*reader)->Next();
    EXPECT_TRUE(next.ok()) << next.status().ToString();
    if (!next.ok() || !next->has_value()) break;
    out.push_back(std::move(**next));
  }
  return out;
}

TEST(SpillTest, EveryTypeRoundTripsWithItsNulls) {
  TempDir dir;
  const fs::path file = dir.path() / "all.spill";
  auto writer = SpillWriter::Open(file, AllTypesSchema());
  ASSERT_TRUE(writer.ok()) << writer.status().ToString();
  std::vector<std::string> expected;
  for (int base : {10, 20}) {
    const Batch batch = AllTypesBatch(base);
    ASSERT_TRUE((*writer)->Write(batch).ok());
    for (auto& row : RowTexts(batch)) expected.push_back(row);
  }
  ASSERT_TRUE((*writer)->Finish().ok());
  EXPECT_GT((*writer)->bytes_written(), 0);
  EXPECT_EQ((*writer)->bytes_written(), static_cast<int64_t>(fs::file_size(file)));
  EXPECT_EQ((*writer)->rows_written(), 8);

  std::vector<std::string> actual;
  for (const Batch& batch : ReadAll(file)) {
    ASSERT_EQ(batch.columns.size(), 8u);
    for (size_t c = 0; c < batch.columns.size(); ++c) {
      EXPECT_TRUE(
          batch.columns[c]->type()->Equals(AllTypesSchema()->field(static_cast<int>(c))->type()));
    }
    for (auto& row : RowTexts(batch)) actual.push_back(row);
  }
  EXPECT_EQ(actual, expected);
}

TEST(SpillTest, BatchesReadBackAreAllocatedFromTheGivenPool) {
  TempDir dir;
  const fs::path file = dir.path() / "pool.spill";
  auto writer = SpillWriter::Open(file, AllTypesSchema());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE((*writer)->Write(AllTypesBatch(10)).ok());
  ASSERT_TRUE((*writer)->Finish().ok());

  arrow::ProxyMemoryPool pool(arrow::default_memory_pool());
  auto reader = SpillReader::Open(file, {}, &pool);
  ASSERT_TRUE(reader.ok()) << reader.status().ToString();
  auto batch = (*reader)->Next();
  ASSERT_TRUE(batch.ok() && batch->has_value());
  EXPECT_GT(pool.max_memory(), 0) << "the batch was allocated elsewhere";
}

TEST(SpillTest, AFileWithoutRowsReadsAsEmpty) {
  TempDir dir;
  const fs::path file = dir.path() / "empty.spill";
  auto writer = SpillWriter::Open(file, AllTypesSchema());
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE((*writer)->Finish().ok());
  EXPECT_TRUE(ReadAll(file).empty());
}

TEST(SpillTest, OnlyTheLiveRowsOfABatchWithASelectionAreWritten) {
  TempDir dir;
  const fs::path file = dir.path() / "sel.spill";
  auto writer = SpillWriter::Open(file, arrow::schema({arrow::field("x", arrow::int64())}));
  ASSERT_TRUE(writer.ok());
  Batch batch = BatchOf({testing::Int64Array({10, 11, 12, 13, 14})});
  auto selection = SelectionVector::Make({1, 3}, 5);
  ASSERT_TRUE(selection.ok());
  batch.selection = std::move(*selection);
  ASSERT_TRUE((*writer)->Write(batch).ok());
  ASSERT_TRUE((*writer)->Finish().ok());
  std::vector<std::string> rows;
  for (const Batch& b : ReadAll(file)) {
    for (auto& r : RowTexts(b)) rows.push_back(r);
  }
  EXPECT_EQ(rows, (std::vector<std::string>{"11|", "13|"}));
}

TEST(SpillTest, TinyBatchesAreBufferedIntoFewerRecordBatches) {
  TempDir dir;
  const fs::path file = dir.path() / "tiny.spill";
  auto writer = SpillWriter::Open(file, arrow::schema({arrow::field("x", arrow::int64())}));
  ASSERT_TRUE(writer.ok());
  const int writes = 1000;
  for (int i = 0; i < writes; ++i) {
    ASSERT_TRUE((*writer)->Write(BatchOf({testing::Int64Array({i, i + 1})})).ok());
  }
  ASSERT_TRUE((*writer)->Finish().ok());
  const std::vector<Batch> batches = ReadAll(file);
  int64_t rows = 0;
  for (const Batch& b : batches) rows += b.length;
  EXPECT_EQ(rows, 2 * writes);
  EXPECT_LT(batches.size(), 100u);
  EXPECT_GT(batches.size(), 1u);
}

TEST(SpillTest, TheLastBufferedRowsAreWrittenByFinish) {
  TempDir dir;
  const fs::path file = dir.path() / "last.spill";
  auto writer = SpillWriter::Open(file, arrow::schema({arrow::field("x", arrow::int64())}));
  ASSERT_TRUE(writer.ok());
  ASSERT_TRUE((*writer)->Write(BatchOf({testing::Int64Array({1, 2, 3})})).ok());
  ASSERT_TRUE((*writer)->Finish().ok());  // far below the flush size: only Finish can write them
  int64_t rows = 0;
  for (const Batch& b : ReadAll(file)) rows += b.length;
  EXPECT_EQ(rows, 3);
}

TEST(SpillTest, FaultsAtEachSiteFailTheOperationWithTheInjectedStatus) {
  TempDir dir;
  const auto schema = arrow::schema({arrow::field("x", arrow::int64())});
  const Batch big = BatchOf({testing::Int64Array(std::vector<int64_t>(2000, 7))});
  const auto fail_at = [](std::string site) -> SpillFault {
    return [site](std::string_view at) {
      return at == site ? arrow::Status::IOError("injected at ", at) : arrow::Status::OK();
    };
  };
  {
    auto writer = SpillWriter::Open(dir.path() / "w.spill", schema, fail_at("spill.write"));
    ASSERT_TRUE(writer.ok());
    const arrow::Status status = (*writer)->Write(big);  // large enough to flush at once
    EXPECT_TRUE(status.IsIOError());
    EXPECT_NE(status.message().find("injected at spill.write"), std::string::npos);
  }
  {
    auto writer = SpillWriter::Open(dir.path() / "f.spill", schema, fail_at("spill.finish"));
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE((*writer)->Write(big).ok());
    const arrow::Status status = (*writer)->Finish();
    EXPECT_TRUE(status.IsIOError());
    EXPECT_NE(status.message().find("injected at spill.finish"), std::string::npos);
  }
  {
    auto writer = SpillWriter::Open(dir.path() / "r.spill", schema);
    ASSERT_TRUE(writer.ok());
    ASSERT_TRUE((*writer)->Write(big).ok());
    ASSERT_TRUE((*writer)->Finish().ok());
    auto reader = SpillReader::Open(dir.path() / "r.spill", fail_at("spill.read"));
    ASSERT_TRUE(reader.ok());
    auto next = (*reader)->Next();
    ASSERT_FALSE(next.ok());
    EXPECT_NE(next.status().message().find("injected at spill.read"), std::string::npos);
  }
}

TEST(SpillTest, OpeningAMissingFileIsAnError) {
  TempDir dir;
  EXPECT_FALSE(SpillReader::Open(dir.path() / "missing.spill").ok());
  EXPECT_FALSE(SpillWriter::Open(dir.path() / "no" / "such" / "dir.spill",
                                 arrow::schema({arrow::field("x", arrow::int64())}))
                   .ok());
}

TEST(SpillDirectoryTest, ItIsUniqueAndGoneAfterTheDestructor) {
  TempDir parent;
  fs::path first_path;
  {
    auto first = SpillDirectory::Create(parent.path());
    auto second = SpillDirectory::Create(parent.path());
    ASSERT_TRUE(first.ok() && second.ok());
    first_path = (*first)->path();
    EXPECT_NE(first_path, (*second)->path());
    EXPECT_TRUE(fs::is_directory(first_path));
    EXPECT_EQ(first_path.parent_path(), parent.path());
    const fs::path a = (*first)->NewFile("join");
    const fs::path b = (*first)->NewFile("join");
    EXPECT_NE(a, b);
    EXPECT_EQ(a.parent_path(), first_path);
    std::ofstream(a) << "data";  // a file left behind goes with the directory
    fs::create_directories(first_path / "nested");
    std::ofstream(first_path / "nested" / "x") << "data";
  }
  EXPECT_FALSE(fs::exists(first_path));
  EXPECT_TRUE(fs::is_empty(parent.path()));
}

TEST(SpillDirectoryTest, ACreateUnderAMissingParentFails) {
  TempDir parent;
  EXPECT_FALSE(SpillDirectory::Create(parent.path() / "missing").ok());
}

// The directory of a query is removed whatever the outcome. The query spills a lot: the 15000
// groups of the aggregate do not fit in 64 KiB.
class SpillCleanupTest : public ::testing::Test {
 protected:
  static constexpr const char* kSpillingQuery =
      "SELECT l_orderkey, SUM(l_extendedprice) FROM lineitem GROUP BY l_orderkey";

  std::unique_ptr<Engine> MakeEngine(EngineOptions options) {
    options.memory_limit_bytes = 64 * 1024;
    options.spill_parent = parent_.path();
    auto engine = std::make_unique<Engine>(std::move(options));
    const arrow::Status st = engine->RegisterDirectory("tests/data/tpch-sf0.01");
    EXPECT_TRUE(st.ok()) << st.ToString();
    return engine;
  }

  // Cancels from inside the spill I/O of `cancel_site`, and checks that the query ends as
  // Cancelled soon after, with nothing left on disk.
  void ExpectCancelEndsPromptly(const std::string& cancel_site, const std::string& sql) {
    Engine* engine_ptr = nullptr;
    std::atomic<bool> cancelled{false};
    Clock::time_point cancelled_at;
    EngineOptions options;
    options.spill_fault = [&](std::string_view site) {
      if (site == cancel_site && !cancelled.exchange(true)) {
        cancelled_at = Clock::now();
        engine_ptr->Cancel();
      }
      return arrow::Status::OK();
    };
    auto engine = MakeEngine(options);
    engine_ptr = engine.get();
    auto result = engine->Execute(sql);
    const Clock::time_point finished = Clock::now();
    ASSERT_TRUE(cancelled.load()) << "the query never reached " << cancel_site;
    ASSERT_FALSE(result.ok()) << "the query finished although it was cancelled";
    EXPECT_TRUE(result.status().IsCancelled()) << result.status().ToString();
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - cancelled_at).count(),
        250);
    EXPECT_TRUE(ParentIsEmpty());
  }

  bool ParentIsEmpty() const { return fs::is_empty(parent_.path()); }

  TempDir parent_;
};

TEST_F(SpillCleanupTest, ASuccessfulQueryLeavesNothingBehind) {
  int writes = 0;
  EngineOptions options;
  options.spill_fault = [&](std::string_view site) {
    if (site == "spill.write") ++writes;
    return arrow::Status::OK();
  };
  auto engine = MakeEngine(options);
  auto result = engine->Execute(kSpillingQuery);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_GT(writes, 0) << "the query was meant to spill";
  EXPECT_TRUE(ParentIsEmpty());
}

TEST_F(SpillCleanupTest, AFaultAtSpillWriteMidQueryReturnsItAndRemovesTheDirectory) {
  int writes = 0;
  EngineOptions options;
  options.spill_fault = [&](std::string_view site) {
    if (site == "spill.write" && ++writes == 5)
      return arrow::Status::IOError("disk full (injected)");
    return arrow::Status::OK();
  };
  auto engine = MakeEngine(options);
  auto result = engine->Execute(kSpillingQuery);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(result.status().IsIOError()) << result.status().ToString();
  EXPECT_NE(result.status().message().find("disk full (injected)"), std::string::npos);
  EXPECT_TRUE(ParentIsEmpty());
}

TEST_F(SpillCleanupTest, AFaultAtSpillReadAndSpillFinishAlsoRemoveTheDirectory) {
  for (const char* site : {"spill.finish", "spill.read"}) {
    EngineOptions options;
    options.spill_fault = [site](std::string_view at) {
      return at == site ? arrow::Status::IOError("injected at ", at) : arrow::Status::OK();
    };
    auto engine = MakeEngine(options);
    auto result = engine->Execute(kSpillingQuery);
    ASSERT_FALSE(result.ok()) << site;
    EXPECT_NE(result.status().message().find(std::string("injected at ") + site), std::string::npos)
        << result.status().ToString();
    EXPECT_TRUE(ParentIsEmpty()) << site;
  }
}

TEST_F(SpillCleanupTest, CancelWhileSpillingEndsPromptlyAndRemovesTheDirectory) {
  ExpectCancelEndsPromptly("spill.write", kSpillingQuery);
}

TEST_F(SpillCleanupTest, CancelWhileMergingPartitionsEndsPromptlyAndRemovesTheDirectory) {
  ExpectCancelEndsPromptly("spill.read", kSpillingQuery);
}

TEST_F(SpillCleanupTest, CancelWhileAJoinPartitionsEndsPromptlyAndRemovesTheDirectory) {
  ExpectCancelEndsPromptly("spill.write",
                           "SELECT COUNT(*) FROM lineitem, orders WHERE l_orderkey = o_orderkey");
}

TEST_F(SpillCleanupTest, ATimeoutWhileSpillingEndsAsCancelledAndRemovesTheDirectory) {
  EngineOptions options;
  options.timeout = std::chrono::milliseconds(100);
  options.spill_fault = [](std::string_view) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return arrow::Status::OK();
  };
  auto engine = MakeEngine(options);
  const Clock::time_point start = Clock::now();
  auto result = engine->Execute(kSpillingQuery);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(result.status().IsCancelled()) << result.status().ToString();
  EXPECT_NE(result.status().message().find("timeout of 100 ms"), std::string::npos)
      << result.status().message();
  EXPECT_LT(elapsed.count(), 1000);
  EXPECT_TRUE(ParentIsEmpty());
}

// What a limit does to operators that cannot spill, and to a join that cannot be split.
class BudgetLimitsTest : public ::testing::Test {
 protected:
  std::unique_ptr<Engine> MakeEngine(int64_t limit) {
    EngineOptions options;
    options.memory_limit_bytes = limit;
    options.spill_parent = parent_.path();
    auto engine = std::make_unique<Engine>(std::move(options));
    const arrow::Status st = engine->RegisterDirectory("tests/data/tpch-sf0.01");
    EXPECT_TRUE(st.ok()) << st.ToString();
    return engine;
  }

  TempDir parent_;
};

TEST_F(BudgetLimitsTest, ASortOverTheBudgetIsResourceExhaustedNamingTheBudgetAndTheRequest) {
  auto engine = MakeEngine(64 * 1024);
  auto result = engine->Execute("SELECT l_orderkey FROM lineitem ORDER BY l_extendedprice");
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status().ToString();
  const std::string message = result.status().message();
  EXPECT_EQ(message.rfind("resource exhausted: Sort #", 0), 0u) << message;
  EXPECT_NE(message.find("needs "), std::string::npos) << message;
  EXPECT_NE(message.find("budget 65536"), std::string::npos) << message;
  EXPECT_TRUE(fs::is_empty(parent_.path()));
}

TEST_F(BudgetLimitsTest, ATopNNeverSpillsAndFitsTheBudget) {
  auto engine = MakeEngine(64 * 1024);
  auto result =
      engine->Execute("SELECT l_orderkey FROM lineitem ORDER BY l_extendedprice DESC LIMIT 10");
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  EXPECT_EQ(result->table->num_rows(), 10);
  for (const OperatorProfile& p : result->profile) EXPECT_EQ(p.spilled_bytes, 0) << p.name;
}

class SkewedJoinTest : public ::testing::Test {
 protected:
  // `build_keys` rows on the smaller side (the build side), 100000 distinct keys on the probe side.
  std::unique_ptr<Engine> MakeEngine(const std::vector<int64_t>& build_keys,
                                     int64_t limit = 64 * 1024) {
    std::vector<int64_t> probe_keys(100000);
    for (size_t i = 0; i < probe_keys.size(); ++i) probe_keys[i] = static_cast<int64_t>(i);
    EngineOptions options;
    options.memory_limit_bytes = limit;
    options.spill_parent = parent_.path();
    auto engine = std::make_unique<Engine>(std::move(options));
    fs::create_directories(data_.path() / "t");
    const fs::path build = testing::WriteParquet(data_.path() / "t" / "b.parquet",
                                                 {testing::Int64Column("k", build_keys)});
    const fs::path probe = testing::WriteParquet(data_.path() / "t" / "p.parquet",
                                                 {testing::Int64Column("k", probe_keys)});
    EXPECT_TRUE(engine->RegisterTable("build_side", {build}).ok());
    EXPECT_TRUE(engine->RegisterTable("probe_side", {probe}).ok());
    return engine;
  }

  static constexpr const char* kJoin =
      "SELECT COUNT(*) FROM probe_side p, build_side b WHERE p.k = b.k";

  TempDir data_;
  TempDir parent_;
};

TEST_F(SkewedJoinTest, EveryBuildKeyEqualIsResourceExhaustedNamingThePartitionDepth) {
  auto engine = MakeEngine(std::vector<int64_t>(20000, 7));
  auto result = engine->Execute(kJoin);
  ASSERT_FALSE(result.ok()) << "a 160 KB build side of one key cannot fit in 64 KiB";
  EXPECT_TRUE(result.status().IsOutOfMemory()) << result.status().ToString();
  const std::string message = result.status().message();
  EXPECT_EQ(message.rfind("resource exhausted: HashJoin #", 0), 0u) << message;
  EXPECT_NE(message.find("partition depth 3"), std::string::npos) << message;
  EXPECT_NE(message.find("budget 65536"), std::string::npos) << message;
  EXPECT_TRUE(fs::is_empty(parent_.path()));
}

TEST_F(SkewedJoinTest, TheSameBuildSideWithDistinctKeysSpillsAndJoins) {
  std::vector<int64_t> keys(20000);
  for (size_t i = 0; i < keys.size(); ++i) keys[i] = static_cast<int64_t>(i * 3);
  auto engine = MakeEngine(keys);
  auto result = engine->Execute(kJoin);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  // The build keys 0, 3, ..., 59997 all occur on the probe side.
  EXPECT_EQ(Cell(*result->table->column(0)->chunk(0), 0), "20000");
  int64_t spilled = 0;
  for (const OperatorProfile& p : result->profile) spilled += p.spilled_bytes;
  EXPECT_GT(spilled, 0);
  EXPECT_TRUE(fs::is_empty(parent_.path()));
}

TEST_F(SkewedJoinTest, AHeavyKeyThatFitsAfterSplittingStillJoinsCorrectly) {
  // 1200 build rows of one key (about 10 KB with its chain) plus 19000 distinct ones: the heavy
  // key's partition fits once the rest is split off.
  std::vector<int64_t> keys(1200, 5);
  for (int64_t i = 0; i < 19000; ++i) keys.push_back(1000 + i);
  auto engine = MakeEngine(keys);
  auto result = engine->Execute(kJoin);
  ASSERT_TRUE(result.ok()) << result.status().ToString();
  // Probe key 5 matches 1200 build rows; probe keys 1000..19999 match one each.
  EXPECT_EQ(Cell(*result->table->column(0)->chunk(0), 0), std::to_string(1200 + 19000));
}

// A join with duplicate keys and NULL keys on both sides, integer and string, gives the same answer
// spilled as unlimited.
TEST(SpilledJoinTest, NullsDuplicatesAndStringKeysJoinLikeAnUnlimitedRun) {
  TempDir data;
  TempDir parent;
  fs::create_directories(data.path() / "t");
  const auto make = [](int64_t rows, int64_t modulus, int null_every) {
    std::vector<int64_t> ints;
    std::vector<std::string> strings;
    std::vector<int64_t> values;
    testing::NullSet nulls;
    for (int64_t i = 0; i < rows; ++i) {
      ints.push_back(i % modulus);
      strings.push_back("key-" + std::to_string(i % modulus));
      values.push_back(i);
      if (i % null_every == 0) nulls.insert(static_cast<size_t>(i));
    }
    return std::vector<testing::Column>{
        {arrow::field("k", arrow::int64()), testing::Int64Array(ints, nulls)},
        {arrow::field("s", arrow::utf8()), testing::StringArray(strings, nulls)},
        {arrow::field("v", arrow::int64()), testing::Int64Array(values)}};
  };
  const fs::path build =
      testing::WriteParquet(data.path() / "t" / "b.parquet", make(9000, 3000, 10));
  const fs::path probe =
      testing::WriteParquet(data.path() / "t" / "p.parquet", make(30000, 4000, 9));
  for (const char* sql : {"SELECT COUNT(*), SUM(b.v), SUM(p.v) FROM probe_side p, build_side b "
                          "WHERE p.k = b.k",
                          "SELECT COUNT(*), SUM(b.v), SUM(p.v) FROM probe_side p, build_side b "
                          "WHERE p.s = b.s",
                          "SELECT COUNT(*) FROM probe_side p, build_side b WHERE p.k = b.k AND "
                          "p.s = b.s"}) {
    std::vector<std::string> answers;
    for (const int64_t limit : {int64_t{0}, int64_t{64} * 1024}) {
      EngineOptions options;
      options.memory_limit_bytes = limit;
      options.spill_parent = parent.path();
      Engine engine(options);
      ASSERT_TRUE(engine.RegisterTable("build_side", {build}).ok());
      ASSERT_TRUE(engine.RegisterTable("probe_side", {probe}).ok());
      auto result = engine.Execute(sql);
      ASSERT_TRUE(result.ok()) << sql << ": " << result.status().ToString();
      std::string answer;
      for (int c = 0; c < result->table->num_columns(); ++c) {
        answer += Cell(*result->table->column(c)->chunk(0), 0) + "|";
      }
      answers.push_back(answer);
      int64_t spilled = 0;
      for (const OperatorProfile& p : result->profile) spilled += p.spilled_bytes;
      EXPECT_EQ(spilled > 0, limit > 0) << sql;
    }
    EXPECT_EQ(answers[0], answers[1]) << sql;
    EXPECT_NE(answers[0].rfind("0|", 0), 0u) << "the join was meant to match rows: " << sql;
  }
  EXPECT_TRUE(fs::is_empty(parent.path()));
}

}  // namespace
}  // namespace quarry
