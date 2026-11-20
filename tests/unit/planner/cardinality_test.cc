#include "planner/cardinality.h"

#include <arrow/scalar.h>
#include <gtest/gtest.h>

#include <memory>
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
