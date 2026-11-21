#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "exec/scan_op.h"
#include "support/exec_helpers.h"
#include "support/parquet_fixture.h"
#include "types/date.h"

namespace quarry {
namespace {

using testing::Drain;
using testing::FindFirstScan;
using testing::Int64Column;
using testing::PlanQuery;
using testing::RowsOf;
using testing::StringColumn;
using testing::StructOfInt64Column;
using testing::TempDir;
using testing::TestContext;
using testing::TotalRows;
using testing::WriteParquet;
using testing::WriteParquetRowGroups;
using testing::WriteParquetWithoutRowGroups;

// events.parquet: 1,000 rows, one per day from 2020-01-01 (day 18262), in ten row groups of 100.
constexpr int32_t kFirstDay = 18262;

const Catalog& Events() {
  static const Catalog* catalog = [] {
    auto* c = new Catalog();
    EXPECT_TRUE(c->AddDirectory("tests/data/pruning").ok());
    return c;
  }();
  return *catalog;
}

struct ScanRun {
  std::vector<std::string> rows;
  ScanReport report;
};

// Plans `sql` against the catalog and runs the scan node of the plan on its own.
ScanRun RunScan(const std::string& sql, const Catalog& catalog, const PlannerOptions& options = {},
                int32_t batch_size = 2048) {
  ScanRun run;
  auto plan = PlanQuery(sql, catalog, options);
  EXPECT_TRUE(plan.ok()) << sql << "\n" << plan.status().ToString();
  if (!plan.ok()) return run;
  const PhysicalScan* scan = FindFirstScan(**plan);
  EXPECT_NE(scan, nullptr);
  if (scan == nullptr) return run;
  ScanOperator op(*scan);
  TestContext context(batch_size);
  auto batches = Drain(op, context.context);
  EXPECT_TRUE(batches.ok()) << batches.status().ToString();
  if (!batches.ok()) return run;
  run.rows = RowsOf(*batches);
  run.report = op.report();
  return run;
}

PlannerOptions NoPruning() {
  PlannerOptions options;
  options.disabled_rules = {"prune_row_groups"};
  return options;
}

std::string Day(int32_t offset) { return "DATE '" + FormatDate(kFirstDay + offset) + "'"; }

TEST(ScanTest, ReadsOnlyProjectedColumns) {
  EXPECT_EQ(RunScan("SELECT id FROM events", Events()).report.columns_decoded, 1);
  EXPECT_EQ(RunScan("SELECT id, amount FROM events", Events()).report.columns_decoded, 2);
  // The filter needs event_date as well as the selected id.
  EXPECT_EQ(RunScan("SELECT id FROM events WHERE event_date < " + Day(5), Events())
                .report.columns_decoded,
            2);
  EXPECT_EQ(RunScan("SELECT id, amount, event_date FROM events", Events()).report.columns_decoded,
            3);
}

TEST(ScanTest, AScanThatNeedsNoColumnReadsTheFirstColumn) {
  PhysicalScan scan;  // no columns: what COUNT(*) needs
  scan.table_name = "events";
  scan.files = {"tests/data/pruning/events.parquet"};
  ScanOperator op(scan);
  TestContext context;
  auto batches = Drain(op, context.context);
  ASSERT_TRUE(batches.ok()) << batches.status().ToString();
  EXPECT_EQ(TotalRows(*batches), 1000);
  for (const Batch& b : *batches) EXPECT_TRUE(b.columns.empty());
  EXPECT_EQ(op.report().columns_decoded, 1);
  EXPECT_EQ(op.report().row_groups_read.size(), 10u);
}

TEST(ScanTest, DecimalColumnsComeOutAsDecimal64) {
  auto plan = PlanQuery("SELECT amount FROM events", Events());
  ASSERT_TRUE(plan.ok());
  ScanOperator op(*FindFirstScan(**plan));
  TestContext context;
  auto batches = Drain(op, context.context);
  ASSERT_TRUE(batches.ok());
  ASSERT_FALSE(batches->empty());
  EXPECT_EQ((*batches)[0].columns[0]->type()->ToString(), "decimal64(12, 2)");
  const std::vector<std::string> rows = RowsOf(*batches);
  ASSERT_EQ(rows.size(), 1000u);
  EXPECT_EQ(rows[1], "0.37");  // 1 * 37 % 10000 hundredths
}

TEST(ScanTest, BatchesAreAtMostBatchSizeRows) {
  auto plan = PlanQuery("SELECT id FROM events", Events());
  ASSERT_TRUE(plan.ok());
  ScanOperator op(*FindFirstScan(**plan));
  TestContext context(7);
  auto batches = Drain(op, context.context);
  ASSERT_TRUE(batches.ok());
  EXPECT_EQ(TotalRows(*batches), 1000);
  for (const Batch& b : *batches) EXPECT_LE(b.active_rows(), 7);
  EXPECT_EQ(RowsOf(*batches).front(), "0");
  EXPECT_EQ(RowsOf(*batches).back(), "999");
}

// Ten row groups of 100 rows: id = i, then a STRUCT of two leaves, then x = 10 i, y = 100 i. The
// columns after the struct are Parquet leaves 3 and 4 but Arrow fields 2 and 3.
TEST(ScanTest, RowGroupPruningUsesTheStatisticsOfTheFilteredColumnAfterANestedColumn) {
  TempDir dir;
  std::vector<int64_t> id;
  std::vector<int64_t> x;
  std::vector<int64_t> y;
  std::vector<int64_t> a(1000, 7);
  for (int64_t i = 0; i < 1000; ++i) {
    id.push_back(i);
    x.push_back(10 * i);
    y.push_back(100 * i);
  }
  const auto file = WriteParquetRowGroups(dir.path() / "n.parquet",
                                          {Int64Column("id", id), StructOfInt64Column("s", a, a),
                                           Int64Column("x", x), Int64Column("y", y)},
                                          100);
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("n", {file}).ok());

  const ScanRun run = RunScan("SELECT id FROM n WHERE x < 1000", catalog);
  EXPECT_EQ(run.report.row_groups_total, 10);
  ASSERT_EQ(run.report.row_groups_read.size(), 1u);
  EXPECT_EQ(run.report.row_groups_read[0].second, 0);
  EXPECT_EQ(run.rows.size(), 100u);

  const ScanRun on_y = RunScan("SELECT id FROM n WHERE y >= 99000", catalog);
  ASSERT_EQ(on_y.report.row_groups_read.size(), 1u);
  EXPECT_EQ(on_y.report.row_groups_read[0].second, 9);
  EXPECT_EQ(on_y.rows.size(), 10u);
}

TEST(ScanTest, AColumnLeftInSeveralChunksIsAnErrorNotAnEmptyArray) {
  auto make = [](std::vector<int64_t> values) {
    arrow::Int64Builder builder;
    EXPECT_TRUE(builder.AppendValues(values).ok());
    std::shared_ptr<arrow::Array> out;
    EXPECT_TRUE(builder.Finish(&out).ok());
    return out;
  };
  const auto one = std::make_shared<arrow::ChunkedArray>(make({1, 2, 3}));
  auto ok = SingleChunk(one, "x");
  ASSERT_TRUE(ok.ok());
  EXPECT_EQ((*ok)->length(), 3);

  const auto two =
      std::make_shared<arrow::ChunkedArray>(arrow::ArrayVector{make({1, 2}), make({3})});
  auto bad = SingleChunk(two, "x");
  ASSERT_FALSE(bad.ok());
  EXPECT_NE(bad.status().message().find("column x has 2 chunks"), std::string::npos)
      << bad.status().message();
}

TEST(ScanTest, TheScanAppliesItsPredicate) {
  const ScanRun run = RunScan("SELECT id FROM events WHERE id BETWEEN 5 AND 7", Events());
  EXPECT_EQ(run.rows, (std::vector<std::string>{"5", "6", "7"}));
}

// The row groups the predicate can touch: group g holds days [100g, 100g + 99].
std::set<int> ExpectedGroups(const std::string& op, int day) {
  std::set<int> groups;
  for (int g = 0; g < 10; ++g) {
    bool any = false;
    for (int d = 100 * g; d < 100 * g + 100; ++d) {
      if ((op == "=" && d == day) || (op == "<" && d < day) || (op == "<=" && d <= day) ||
          (op == ">" && d > day) || (op == ">=" && d >= day)) {
        any = true;
      }
    }
    if (any) groups.insert(g);
  }
  return groups;
}

std::set<int> GroupsRead(const ScanReport& report) {
  std::set<int> groups;
  for (const auto& entry : report.row_groups_read) groups.insert(entry.second);
  return groups;
}

TEST(ScanTest, PrunesRowGroupsOnBoundaries) {
  struct Case {
    std::string where;
    std::set<int> groups;
  };
  const std::vector<Case> cases = {
      {"event_date = " + Day(300), {3}},                                // the minimum of group 3
      {"event_date = " + Day(399), {3}},                                // its maximum
      {"event_date = " + Day(299), {2}},                                // one day below its minimum
      {"event_date = " + Day(400), {4}},                                // one day above its maximum
      {"event_date BETWEEN " + Day(399) + " AND " + Day(400), {3, 4}},  // spans groups 3 and 4
      {"event_date BETWEEN " + Day(350) + " AND " + Day(450), {3, 4}},
      {"event_date < " + Day(0), {}},  // below the minimum of group 0
      {"event_date <= " + Day(0), {0}},
      {"event_date > " + Day(999), {}},
      {"event_date >= " + Day(999), {9}},
      {"event_date >= " + Day(399), {3, 4, 5, 6, 7, 8, 9}},
      {"event_date > " + Day(399), {4, 5, 6, 7, 8, 9}},
      {"event_date < " + Day(300), {0, 1, 2}},
      {"event_date <= " + Day(300), {0, 1, 2, 3}},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.where);
    const std::string sql = "SELECT id FROM events WHERE " + c.where;
    const ScanRun pruned = RunScan(sql, Events());
    const ScanRun full = RunScan(sql, Events(), NoPruning());
    EXPECT_EQ(GroupsRead(pruned.report), c.groups);
    EXPECT_EQ(full.report.row_groups_read.size(), 10u);
    EXPECT_EQ(pruned.report.row_groups_total, 10);
    EXPECT_EQ(pruned.rows, full.rows);
  }
}

TEST(ScanTest, EveryComparisonAroundTheBoundariesOfARowGroup) {
  const std::vector<std::string> ops = {"=", "<", "<=", ">", ">="};
  for (const std::string& op : ops) {
    for (const int day : {299, 300, 301, 398, 399, 400}) {
      SCOPED_TRACE(op + " " + std::to_string(day));
      const std::string sql = "SELECT id FROM events WHERE event_date " + op + " " + Day(day);
      const ScanRun pruned = RunScan(sql, Events());
      const ScanRun full = RunScan(sql, Events(), NoPruning());
      EXPECT_EQ(GroupsRead(pruned.report), ExpectedGroups(op, day));
      EXPECT_EQ(pruned.rows, full.rows);
    }
  }
}

TEST(ScanTest, AFlippedComparisonPrunesToo) {
  const ScanRun run =
      RunScan("SELECT id FROM events WHERE " + Day(399) + " = event_date", Events());
  EXPECT_EQ(GroupsRead(run.report), (std::set<int>{3}));
  EXPECT_EQ(run.rows, (std::vector<std::string>{"18661|399"}));  // event_date, id
}

TEST(ScanTest, ANonPrunablePredicateReadsEveryRowGroup) {
  const ScanRun run = RunScan("SELECT id FROM events WHERE event_date <> " + Day(5), Events());
  EXPECT_EQ(run.report.row_groups_read.size(), 10u);
  EXPECT_EQ(run.rows.size(), 999u);
}

TEST(ScanTest, AFileWithZeroRowGroupsIsAnEmptyTable) {
  TempDir dir;
  const auto file =
      WriteParquetWithoutRowGroups(dir.path() / "empty.parquet", {Int64Column("x", {})});
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("empty", {file}).ok());
  const ScanRun run = RunScan("SELECT x FROM empty", catalog);
  EXPECT_TRUE(run.rows.empty());
  EXPECT_EQ(run.report.row_groups_total, 0);
  EXPECT_TRUE(run.report.row_groups_read.empty());
}

TEST(ScanTest, FilesOfOneTableMayOrderTheirColumnsDifferently) {
  TempDir dir;
  const auto first = WriteParquet(dir.path() / "a.parquet",
                                  {Int64Column("x", {1, 2}), StringColumn("y", {"a", "b"})});
  const auto second = WriteParquet(dir.path() / "b.parquet",
                                   {StringColumn("y", {"c", "d"}), Int64Column("x", {3, 4})});
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("t", {first, second}).ok());
  // The scan outputs x then y, the order of the first file, whatever order each file has.
  const ScanRun run = RunScan("SELECT y, x FROM t", catalog);
  EXPECT_EQ(run.rows, (std::vector<std::string>{"1|a", "2|b", "3|c", "4|d"}));
  EXPECT_EQ(run.report.row_groups_read.size(), 2u);
}

TEST(ScanTest, ARowGroupsStatisticsAreMatchedByColumnNameAcrossFiles) {
  // Pruning on x must use x's statistics in both files, although x is column 0 in one and 1 in the
  // other.
  TempDir dir;
  const auto first = WriteParquet(dir.path() / "a.parquet",
                                  {Int64Column("x", {1, 2}), StringColumn("y", {"a", "b"})});
  const auto second = WriteParquet(dir.path() / "b.parquet",
                                   {StringColumn("y", {"c", "d"}), Int64Column("x", {30, 40})});
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("t", {first, second}).ok());
  const ScanRun run = RunScan("SELECT y FROM t WHERE x = 40", catalog);
  EXPECT_EQ(run.rows, (std::vector<std::string>{"40|d"}));
  EXPECT_EQ(run.report.row_groups_read, (std::vector<std::pair<int, int>>{{1, 0}}));
}

}  // namespace
}  // namespace quarry
