#include "planner/cardinality.h"

#include <arrow/scalar.h>
#include <arrow/type.h>
#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "binder/catalog.h"
#include "expr/bound_expr.h"
#include "plan/logical.h"

namespace quarry {
namespace {

const LogicalType kInt = LogicalType::Int32();

ExprPtr Must(arrow::Result<ExprPtr> expr) { return std::move(expr).ValueOrDie(); }
ExprPtr Col(int index) { return MakeColumn(index, kInt); }
ExprPtr Lit(int32_t v) { return Must(MakeLiteral(std::make_shared<arrow::Int32Scalar>(v))); }
ExprPtr Cmp(BinaryOp op, int column, int32_t value) {
  return Must(MakeBinary(op, Col(column), Lit(value)));
}

std::shared_ptr<arrow::Scalar> Int(int32_t v) { return std::make_shared<arrow::Int32Scalar>(v); }

TableInfo MakeTable(const std::string& name, int64_t rows) {
  TableInfo table;
  table.name = name;
  table.row_count = rows;
  return table;
}

// A table of one file whose row groups give column a these (min, max) pairs; column b has no
// statistics. `rows` is the table's row count. The scalars are Int32 unless `as_float`.
TableInfo MakeStatsTable(const std::string& name, int64_t rows,
                         const std::vector<std::pair<int32_t, int32_t>>& groups,
                         bool as_float = false) {
  TableInfo table = MakeTable(name, rows);
  for (const char* column : {"a", "b"}) {
    ColumnInfo info;
    info.name = column;
    table.columns.push_back(info);
  }
  ParquetFileInfo file;
  const std::shared_ptr<arrow::DataType> type = as_float ? arrow::float32() : arrow::int32();
  file.schema = arrow::schema({arrow::field("A", type), arrow::field("b", type)});
  file.num_rows = rows;
  for (const auto& [low, high] : groups) {
    RowGroupStats group;
    group.num_rows = rows / static_cast<int64_t>(groups.size());
    ColumnMinMax stats;
    if (as_float) {
      stats.min = std::make_shared<arrow::FloatScalar>(static_cast<float>(low));
      stats.max = std::make_shared<arrow::FloatScalar>(static_cast<float>(high));
    } else {
      stats.min = Int(low);
      stats.max = Int(high);
    }
    group.columns = {stats, std::nullopt};
    file.row_groups.push_back(std::move(group));
  }
  file.num_row_groups = static_cast<int>(file.row_groups.size());
  table.file_info.push_back(std::move(file));
  return table;
}

// A scan of columns a and b of `table`.
std::unique_ptr<LogicalScan> MakeScan(const TableInfo& table, std::vector<ExprPtr> filters = {}) {
  auto scan = std::make_unique<LogicalScan>();
  scan->table = &table;
  scan->table_name = table.name;
  scan->table_column_names = {"a", "b"};
  scan->table_column_types = {kInt, kInt};
  scan->columns = {0, 1};
  scan->filters = std::move(filters);
  return scan;
}

// `left JOIN right ON left.a = right.a`, the first table on the left.
double JoinOn(const TableInfo& left, const TableInfo& right) {
  auto join = std::make_unique<LogicalJoin>();
  join->left = MakeScan(left);
  join->right = MakeScan(right);
  join->left_keys.push_back(Col(0));
  join->right_keys.push_back(Col(0));
  return EstimateRows(*join);
}

double RowsWith(ExprPtr filter, int64_t rows = 1000) {
  const TableInfo table = MakeTable("t", rows);
  std::vector<ExprPtr> filters;
  filters.push_back(std::move(filter));
  return EstimateRows(*MakeScan(table, std::move(filters)));
}

TEST(CardinalityTest, ScanWithoutFiltersIsTheRowCount) {
  const TableInfo table = MakeTable("t", 1000);
  EXPECT_DOUBLE_EQ(EstimateRows(*MakeScan(table)), 1000.0);
}

TEST(CardinalityTest, EqualityKeepsATenth) {
  EXPECT_NEAR(RowsWith(Cmp(BinaryOp::kEq, 0, 5)), 100.0, 1e-9);
}

TEST(CardinalityTest, RangeComparisonsKeepThreeTenths) {
  for (const BinaryOp op : {BinaryOp::kLt, BinaryOp::kLe, BinaryOp::kGt, BinaryOp::kGe}) {
    EXPECT_NEAR(RowsWith(Cmp(op, 0, 5)), 300.0, 1e-9) << BinaryOpName(op);
  }
}

TEST(CardinalityTest, BetweenKeepsThreeTenths) {
  EXPECT_NEAR(RowsWith(Must(MakeBetween(Col(0), Lit(1), Lit(9)))), 300.0, 1e-9);
}

TEST(CardinalityTest, InListKeepsATenthPerValueUpToHalf) {
  EXPECT_NEAR(RowsWith(Must(MakeInList(Col(0), {Int(1), Int(2), Int(3)}))), 300.0, 1e-9);
  EXPECT_NEAR(RowsWith(Must(MakeInList(Col(0), {Int(1)}))), 100.0, 1e-9);
  EXPECT_NEAR(
      RowsWith(Must(MakeInList(Col(0), {Int(1), Int(2), Int(3), Int(4), Int(5), Int(6), Int(7)}))),
      500.0, 1e-9);
}

TEST(CardinalityTest, NullTests) {
  EXPECT_NEAR(RowsWith(Must(MakeIsNull(Col(0), false))), 100.0, 1e-9);
  EXPECT_NEAR(RowsWith(Must(MakeIsNull(Col(0), true))), 900.0, 1e-9);
}

TEST(CardinalityTest, OrCombinesAsInclusionExclusion) {
  // 0.1 + 0.3 - 0.1 * 0.3 = 0.37
  ExprPtr either =
      Must(MakeBinary(BinaryOp::kOr, Cmp(BinaryOp::kEq, 0, 1), Cmp(BinaryOp::kLt, 1, 2)));
  EXPECT_NEAR(RowsWith(std::move(either)), 370.0, 1e-9);
}

TEST(CardinalityTest, NotIsTheComplement) {
  EXPECT_NEAR(RowsWith(Must(MakeNot(Cmp(BinaryOp::kLt, 0, 5)))), 700.0, 1e-9);
}

TEST(CardinalityTest, AndMultipliesAndSoDoSeparateFilters) {
  ExprPtr both =
      Must(MakeBinary(BinaryOp::kAnd, Cmp(BinaryOp::kEq, 0, 1), Cmp(BinaryOp::kLt, 1, 2)));
  EXPECT_NEAR(RowsWith(std::move(both)), 30.0, 1e-9);

  const TableInfo table = MakeTable("t", 1000);
  std::vector<ExprPtr> filters;
  filters.push_back(Cmp(BinaryOp::kEq, 0, 1));
  filters.push_back(Cmp(BinaryOp::kLt, 1, 2));
  EXPECT_NEAR(EstimateRows(*MakeScan(table, std::move(filters))), 30.0, 1e-9);
}

TEST(CardinalityTest, NotEqualKeepsNineTenths) {
  EXPECT_NEAR(RowsWith(Cmp(BinaryOp::kNe, 0, 5)), 900.0, 1e-9);
}

TEST(CardinalityTest, JoinFormulaDividesByTheSmallerBaseTable) {
  const TableInfo small = MakeTable("small", 1000);
  const TableInfo large = MakeTable("large", 10000);
  auto join = std::make_unique<LogicalJoin>();
  join->left = MakeScan(large);
  join->right = MakeScan(small);
  join->left_keys.push_back(Col(0));
  join->right_keys.push_back(Col(0));
  EXPECT_NEAR(EstimateRows(*join), 10000.0, 1e-6);  // 10000 * 1000 / 1000
}

TEST(CardinalityTest, JoinFormulaUsesBaseRowsNotFilteredRows) {
  const TableInfo small = MakeTable("small", 1000);
  const TableInfo large = MakeTable("large", 10000);
  auto join = std::make_unique<LogicalJoin>();
  std::vector<ExprPtr> filters;
  filters.push_back(Cmp(BinaryOp::kEq, 1, 7));
  join->left = MakeScan(large);
  join->right = MakeScan(small, std::move(filters));  // 100 rows of a base table of 1000
  join->left_keys.push_back(Col(0));
  join->right_keys.push_back(Col(0));
  EXPECT_NEAR(EstimateRows(*join), 1000.0, 1e-6);  // 10000 * 100 / 1000
}

TEST(CardinalityTest, EachExtraKeyPairKeepsATenth) {
  const TableInfo small = MakeTable("small", 1000);
  const TableInfo large = MakeTable("large", 10000);
  auto join = std::make_unique<LogicalJoin>();
  join->left = MakeScan(large);
  join->right = MakeScan(small);
  join->left_keys.push_back(Col(0));
  join->right_keys.push_back(Col(0));
  join->left_keys.push_back(Col(1));
  join->right_keys.push_back(Col(1));
  EXPECT_NEAR(EstimateRows(*join), 1000.0, 1e-6);  // 10000 * 0.1
}

TEST(CardinalityTest, TwoForeignKeysOnASmallDomainAreNotAUniqueKey) {
  // Both columns hold values 0..24 (like c_nationkey and s_nationkey, which reference nation): the
  // join is many-to-many, so the divisor is the 25 values, not the 1000 rows of the smaller table.
  const TableInfo small = MakeStatsTable("small", 1000, {{0, 24}});
  const TableInfo large = MakeStatsTable("large", 10000, {{0, 24}});
  EXPECT_NEAR(JoinOn(large, small), 10000.0 * 1000.0 / 25.0, 1e-6);
  EXPECT_DOUBLE_EQ(KeyDomain(*MakeScan(large), *Col(0)), 25.0);
}

TEST(CardinalityTest, AKeyWhoseDomainIsWiderThanTheKeyTableKeepsTheBaseRowDivisor) {
  // A sparse primary key: 1000 rows over 1..6000, referenced by 10000 rows over the same domain.
  const TableInfo small = MakeStatsTable("small", 1000, {{1, 6000}});
  const TableInfo large = MakeStatsTable("large", 10000, {{1, 6000}});
  EXPECT_NEAR(JoinOn(large, small), 10000.0, 1e-6);  // 10000 * 1000 / 1000
}

TEST(CardinalityTest, TheDomainSpansEveryRowGroup) {
  const TableInfo t = MakeStatsTable("t", 1000, {{10, 14}, {0, 9}, {20, 24}});
  EXPECT_DOUBLE_EQ(KeyDomain(*MakeScan(t), *Col(0)), 25.0);  // 0..24
}

TEST(CardinalityTest, ADomainCapNeedsStatisticsOnTheWiderSide) {
  // The larger of the two bounds is used, so one unknown bound leaves the base-row divisor.
  const TableInfo known = MakeStatsTable("known", 10000, {{0, 24}});
  const TableInfo unknown = MakeTable("unknown", 1000);
  EXPECT_NEAR(JoinOn(known, unknown), 10000.0, 1e-6);
  EXPECT_NEAR(JoinOn(unknown, known), 10000.0, 1e-6);
  EXPECT_TRUE(std::isinf(KeyDomain(*MakeScan(unknown), *Col(0))));
}

TEST(CardinalityTest, AKeyWithoutStatisticsInOneRowGroupHasNoDomain) {
  TableInfo t = MakeStatsTable("t", 1000, {{0, 24}, {0, 24}});
  t.file_info[0].row_groups[1].columns[0] = std::nullopt;
  EXPECT_TRUE(std::isinf(KeyDomain(*MakeScan(t), *Col(0))));
}

TEST(CardinalityTest, ANonIntegerKeyKeepsTheBaseRowDivisor) {
  const TableInfo small = MakeStatsTable("small", 1000, {{0, 24}}, /*as_float=*/true);
  const TableInfo large = MakeStatsTable("large", 10000, {{0, 24}}, /*as_float=*/true);
  EXPECT_TRUE(std::isinf(KeyDomain(*MakeScan(large), *Col(0))));
  EXPECT_NEAR(JoinOn(large, small), 10000.0, 1e-6);
}

TEST(CardinalityTest, AKeyThatDoesNotTraceToAScanHasNoDomain) {
  const TableInfo t = MakeStatsTable("t", 1000, {{0, 24}});
  auto aggregate = std::make_unique<LogicalAggregate>();
  aggregate->child = MakeScan(t);
  aggregate->groups.push_back(Must(MakeBinary(BinaryOp::kAdd, Col(0), Lit(1))));
  EXPECT_TRUE(std::isinf(KeyDomain(*aggregate, *Col(0))));
}

TEST(CardinalityTest, TheDomainFollowsAKeyThroughAJoin) {
  const TableInfo a = MakeStatsTable("a", 50, {{0, 4}});
  const TableInfo b = MakeStatsTable("b", 7000, {{1, 100}});
  auto inner = std::make_unique<LogicalJoin>();
  inner->left = MakeScan(a);
  inner->right = MakeScan(b);
  inner->left_keys.push_back(Col(0));
  inner->right_keys.push_back(Col(0));
  EXPECT_DOUBLE_EQ(KeyDomain(*inner, *Col(0)), 5.0);    // from a
  EXPECT_DOUBLE_EQ(KeyDomain(*inner, *Col(2)), 100.0);  // from b
}

TEST(CardinalityTest, KeyBaseRowsFollowsTheKeyColumnThroughAJoin) {
  const TableInfo a = MakeTable("a", 50);
  const TableInfo b = MakeTable("b", 7000);
  auto inner = std::make_unique<LogicalJoin>();
  inner->left = MakeScan(a);
  inner->right = MakeScan(b);
  inner->left_keys.push_back(Col(0));
  inner->right_keys.push_back(Col(0));
  // Columns 0-1 come from a, 2-3 from b.
  EXPECT_DOUBLE_EQ(KeyBaseRows(*inner, *Col(1)), 50.0);
  EXPECT_DOUBLE_EQ(KeyBaseRows(*inner, *Col(3)), 7000.0);
}

TEST(CardinalityTest, KeyBaseRowsFallsBackToTheSideEstimateOffAScan) {
  const TableInfo t = MakeTable("t", 1000);
  auto aggregate = std::make_unique<LogicalAggregate>();
  aggregate->child = MakeScan(t);
  aggregate->groups.push_back(Must(MakeBinary(BinaryOp::kAdd, Col(0), Lit(1))));
  EXPECT_DOUBLE_EQ(KeyBaseRows(*aggregate, *Col(0)), EstimateRows(*aggregate));
}

TEST(CardinalityTest, OtherNodes) {
  const TableInfo t = MakeTable("t", 1000);
  auto filter = std::make_unique<LogicalFilter>();
  filter->child = MakeScan(t);
  filter->predicate = Cmp(BinaryOp::kEq, 0, 3);
  EXPECT_NEAR(EstimateRows(*filter), 100.0, 1e-9);

  auto limit = std::make_unique<LogicalLimit>();
  limit->child = MakeScan(t);
  limit->n = 10;
  EXPECT_DOUBLE_EQ(EstimateRows(*limit), 10.0);
  limit->n = 5000;
  EXPECT_DOUBLE_EQ(EstimateRows(*limit), 1000.0);

  auto cross = std::make_unique<LogicalCrossProduct>();
  const TableInfo u = MakeTable("u", 20);
  cross->children.push_back(MakeScan(t));
  cross->children.push_back(MakeScan(u));
  EXPECT_DOUBLE_EQ(EstimateRows(*cross), 20000.0);

  auto global = std::make_unique<LogicalAggregate>();
  global->child = MakeScan(t);
  EXPECT_DOUBLE_EQ(EstimateRows(*global), 1.0);
  auto grouped = std::make_unique<LogicalAggregate>();
  grouped->child = MakeScan(t);
  grouped->groups.push_back(Col(0));
  EXPECT_NEAR(EstimateRows(*grouped), 100.0, 1e-9);
}

TEST(CardinalityTest, EstimateJoinRowsWithoutKeysIsTheProduct) {
  EXPECT_DOUBLE_EQ(EstimateJoinRows(30, 40, {}), 1200.0);
  EXPECT_NEAR(EstimateJoinRows(30, 40, {10, 100}), 12.0, 1e-9);
}

}  // namespace
}  // namespace quarry
