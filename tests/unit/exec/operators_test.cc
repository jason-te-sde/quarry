#include "exec/operators.h"

#include <arrow/scalar.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "exec/build_operators.h"
#include "support/exec_helpers.h"
#include "support/vector_source.h"

namespace quarry {
namespace {

using testing::BatchOf;
using testing::BoolArray;
using testing::Decimal64Array;
using testing::DoubleArray;
using testing::Drain;
using testing::Int32Array;
using testing::Int64Array;
using testing::RowsOf;
using testing::Source;
using testing::StringArray;
using testing::TestContext;
using testing::TotalRows;

using Rows = std::vector<std::string>;

ExprPtr Col(int index, const LogicalType& type = LogicalType::Int64()) {
  return MakeColumn(index, type);
}

ExprPtr Lit(int64_t value) {
  return MakeLiteral(std::make_shared<arrow::Int64Scalar>(value)).ValueOrDie();
}

ExprPtr Bin(BinaryOp op, ExprPtr l, ExprPtr r) {
  auto result = MakeBinary(op, std::move(l), std::move(r));
  EXPECT_TRUE(result.ok()) << result.status().ToString();
  return std::move(*result);
}

SortKey Key(int column, bool ascending = true, bool nulls_first = false,
            const LogicalType& type = LogicalType::Int64()) {
  SortKey key;
  key.expr = Col(column, type);
  key.ascending = ascending;
  key.nulls_first = nulls_first;
  return key;
}

std::vector<std::string> RunOp(std::unique_ptr<Operator> op, int32_t batch_size = 2048,
                               int64_t* batches_out = nullptr) {
  TestContext context(batch_size);
  auto batches = Drain(*op, context.context);
  EXPECT_TRUE(batches.ok()) << batches.status().ToString();
  if (!batches.ok()) return {};
  for (const Batch& b : *batches)
    EXPECT_GT(b.active_rows(), 0) << "an operator returned an empty batch";
  if (batches_out != nullptr) *batches_out = static_cast<int64_t>(batches->size());
  return RowsOf(*batches);
}

// ---- filter and project ----

TEST(FilterOperatorTest, SetsASelectionVectorOverTheSameColumns) {
  PhysicalFilter node;
  node.predicate = Bin(BinaryOp::kGt, Col(0), Lit(3));
  auto source = Source(
      {BatchOf({Int64Array({1, 2, 3, 4, 5, 6}), StringArray({"a", "b", "c", "d", "e", "f"})})});
  auto op = MakeFilterOperator(node, std::move(source));
  TestContext context;
  auto batches = Drain(*op, context.context);
  ASSERT_TRUE(batches.ok());
  ASSERT_EQ(batches->size(), 1u);
  ASSERT_TRUE((*batches)[0].selection.has_value());
  EXPECT_EQ((*batches)[0].length, 6);  // nothing was copied
  EXPECT_EQ(RowsOf(*batches), (Rows{"4|d", "5|e", "6|f"}));
}

TEST(FilterOperatorTest, NullAndFalseDropTheRowAndEmptyBatchesAreSkipped) {
  PhysicalFilter node;
  node.predicate = Bin(BinaryOp::kGt, Col(0), Lit(3));
  auto source = Source({BatchOf({Int64Array({1, 2})}), BatchOf({Int64Array({0, 9, 3}, {0})}),
                        BatchOf({Int64Array({0, 0}, {0, 1})}), BatchOf({Int64Array({7})})});
  int64_t batches = 0;
  EXPECT_EQ(RunOp(MakeFilterOperator(node, std::move(source)), 2048, &batches), (Rows{"9", "7"}));
  EXPECT_EQ(batches, 2);
}

TEST(FilterOperatorTest, ASecondFilterNarrowsAnExistingSelection) {
  PhysicalFilter first;
  first.predicate = Bin(BinaryOp::kGt, Col(0), Lit(2));
  PhysicalFilter second;
  second.predicate = Bin(BinaryOp::kLt, Col(0), Lit(6));
  auto op = MakeFilterOperator(
      second, MakeFilterOperator(first, Source({BatchOf({Int64Array({1, 2, 3, 4, 5, 6, 7})})})));
  EXPECT_EQ(RunOp(std::move(op)), (Rows{"3", "4", "5"}));
}

TEST(ProjectOperatorTest, EvaluatesExpressionsOverTheLiveRows) {
  PhysicalProject node;
  node.exprs.push_back(Bin(BinaryOp::kMul, Col(0), Lit(10)));
  node.exprs.push_back(Col(1, LogicalType::Varchar()));
  node.names = {"x", "y"};
  PhysicalFilter filter;
  filter.predicate = Bin(BinaryOp::kNe, Col(0), Lit(2));
  auto op = MakeProjectOperator(
      node, MakeFilterOperator(
                filter, Source({BatchOf({Int64Array({1, 2, 3}), StringArray({"a", "b", "c"})})})));
  EXPECT_EQ(RunOp(std::move(op)), (Rows{"10|a", "30|c"}));
}

// ---- hash join ----

struct JoinSpec {
  std::vector<ExprPtr> probe_keys;
  std::vector<ExprPtr> build_keys;
  ExprPtr residual;
  std::vector<JoinOutputColumn> output;
  int probe_width = 2;
  int build_width = 2;
};

std::unique_ptr<PhysicalHashJoin> MakeJoin(JoinSpec spec) {
  auto owner = std::make_unique<PhysicalHashJoin>();
  PhysicalHashJoin& node = *owner;
  node.probe_keys = std::move(spec.probe_keys);
  node.build_keys = std::move(spec.build_keys);
  node.residual = std::move(spec.residual);
  node.output_columns = std::move(spec.output);
  node.probe = std::make_unique<PhysicalScan>();
  node.build = std::make_unique<PhysicalScan>();
  node.probe->output_types.assign(static_cast<size_t>(spec.probe_width), LogicalType::Int64());
  node.build->output_types.assign(static_cast<size_t>(spec.build_width), LogicalType::Int64());
  return owner;
}

std::vector<JoinOutputColumn> AllColumns(int probe_width, int build_width) {
  std::vector<JoinOutputColumn> out;
  for (int i = 0; i < probe_width; ++i) out.push_back({false, i});
  for (int i = 0; i < build_width; ++i) out.push_back({true, i});
  return out;
}

Rows Sorted(Rows rows) {
  std::sort(rows.begin(), rows.end());
  return rows;
}

JoinSpec SingleKey() {
  JoinSpec spec;
  spec.probe_keys.push_back(Col(0));
  spec.build_keys.push_back(Col(0));
  spec.output = AllColumns(2, 2);
  return spec;
}

TEST(HashJoinOperatorTest, InnerJoinOnOneKey) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  auto probe = Source({BatchOf({Int64Array({1, 2, 3, 4}), Int64Array({10, 20, 30, 40})})});
  auto build = Source({BatchOf({Int64Array({2, 4, 5}), Int64Array({200, 400, 500})})});
  EXPECT_EQ(Sorted(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build)))),
            (Rows{"2|20|2|200", "4|40|4|400"}));
}

TEST(HashJoinOperatorTest, DuplicateKeysOnBothSidesMultiply) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  auto probe = Source({BatchOf({Int64Array({7, 7, 8}), Int64Array({1, 2, 3})})});
  auto build = Source({BatchOf({Int64Array({7, 7, 7}), Int64Array({100, 200, 300})}),
                       BatchOf({Int64Array({8}), Int64Array({800})})});
  const Rows rows = RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build)));
  EXPECT_EQ(rows.size(), 7u);
  EXPECT_EQ(Sorted(rows), (Rows{"7|1|7|100", "7|1|7|200", "7|1|7|300", "7|2|7|100", "7|2|7|200",
                                "7|2|7|300", "8|3|8|800"}));
}

TEST(HashJoinOperatorTest, MultiKeyJoin) {
  JoinSpec spec;
  spec.probe_keys.push_back(Col(0));
  spec.probe_keys.push_back(Col(1));
  spec.build_keys.push_back(Col(0));
  spec.build_keys.push_back(Col(1));
  spec.output = AllColumns(3, 3);
  spec.probe_width = spec.build_width = 3;
  auto node_owner = MakeJoin(std::move(spec));
  const PhysicalHashJoin& node = *node_owner;
  auto probe =
      Source({BatchOf({Int64Array({1, 1, 2}), Int64Array({1, 2, 1}), Int64Array({10, 20, 30})})});
  auto build =
      Source({BatchOf({Int64Array({1, 2, 2}), Int64Array({2, 1, 2}), Int64Array({-1, -2, -3})})});
  EXPECT_EQ(Sorted(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build)))),
            (Rows{"1|2|20|1|2|-1", "2|1|30|2|1|-2"}));
}

TEST(HashJoinOperatorTest, NullKeysNeverJoin) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  auto probe = Source({BatchOf({Int64Array({0, 1}, {0}), Int64Array({10, 20})})});
  auto build = Source({BatchOf({Int64Array({0, 1}, {0}), Int64Array({100, 200})})});
  EXPECT_EQ(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build))),
            (Rows{"1|20|1|200"}));
}

TEST(HashJoinOperatorTest, AnEmptyBuildSideGivesNoRowsAndLeavesTheProbeUnread) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  int probe_pulls = 0;
  auto probe = Source({BatchOf({Int64Array({1}), Int64Array({1})})}, &probe_pulls);
  auto build = Source({});
  EXPECT_TRUE(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build))).empty());
  EXPECT_EQ(probe_pulls, 0);
}

TEST(HashJoinOperatorTest, AnEmptyProbeSideGivesNoRows) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  auto probe = Source({});
  auto build = Source({BatchOf({Int64Array({1}), Int64Array({1})})});
  EXPECT_TRUE(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build))).empty());
}

TEST(HashJoinOperatorTest, TheResidualIndexesProbeColumnsThenBuildColumns) {
  JoinSpec spec = SingleKey();
  // probe: (k, p); build: (k, b). Keep matches where p < b: columns #1 and #3 of probe ++ build.
  spec.residual = Bin(BinaryOp::kLt, Col(1), Col(3));
  auto node_owner = MakeJoin(std::move(spec));
  const PhysicalHashJoin& node = *node_owner;
  auto probe = Source({BatchOf({Int64Array({1, 1, 2}), Int64Array({5, 50, 5})})});
  auto build = Source({BatchOf({Int64Array({1, 2}), Int64Array({10, 1})})});
  EXPECT_EQ(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build))),
            (Rows{"1|5|1|10"}));
}

TEST(HashJoinOperatorTest, OutputColumnsCanBeChosenAndReordered) {
  JoinSpec spec = SingleKey();
  spec.output = {{true, 1}, {false, 1}};
  auto node_owner = MakeJoin(std::move(spec));
  const PhysicalHashJoin& node = *node_owner;
  auto probe = Source({BatchOf({Int64Array({1, 2}), Int64Array({10, 20})})});
  auto build = Source({BatchOf({Int64Array({2, 1}), Int64Array({200, 100})})});
  EXPECT_EQ(Sorted(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build)))),
            (Rows{"100|10", "200|20"}));
}

TEST(HashJoinOperatorTest, ALargeFanOutIsSplitIntoBatches) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  std::vector<int64_t> keys(40, 1);
  std::vector<int64_t> payload(40, 0);
  auto probe = Source({BatchOf({Int64Array(keys), Int64Array(payload)})});
  auto build = Source({BatchOf({Int64Array(keys), Int64Array(payload)})});
  int64_t batches = 0;
  const Rows rows =
      RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build)), 100, &batches);
  EXPECT_EQ(rows.size(), 1600u);
  EXPECT_EQ(batches, 16);
}

TEST(HashJoinOperatorTest, ProbeBatchesWithASelectionAreJoinedByTheirLiveRows) {
  auto node_owner = MakeJoin(SingleKey());
  const PhysicalHashJoin& node = *node_owner;
  PhysicalFilter filter;
  filter.predicate = Bin(BinaryOp::kGt, Col(1), Lit(10));
  auto probe = MakeFilterOperator(
      filter, Source({BatchOf({Int64Array({1, 2, 3}), Int64Array({10, 20, 30})})}));
  auto build = Source({BatchOf({Int64Array({1, 2, 3}), Int64Array({-1, -2, -3})})});
  EXPECT_EQ(Sorted(RunOp(MakeHashJoinOperator(node, std::move(probe), std::move(build)))),
            (Rows{"2|20|2|-2", "3|30|3|-3"}));
}

// ---- hash aggregate ----

std::unique_ptr<PhysicalHashAggregate> MakeAggregate(std::vector<ExprPtr> groups,
                                                     std::vector<AggregateExpr> aggs) {
  auto owner = std::make_unique<PhysicalHashAggregate>();
  PhysicalHashAggregate& node = *owner;
  node.groups = std::move(groups);
  node.aggregates = std::move(aggs);
  return owner;
}

AggregateExpr Agg(AggFunc func, ExprPtr arg) {
  AggregateExpr a;
  a.func = func;
  if (arg != nullptr) {
    auto result = AggregateResultType(func, arg->type);
    EXPECT_TRUE(result.ok()) << result.status().ToString();
    a.result_type = *result;
  } else {
    a.result_type = LogicalType::Int64();
  }
  a.arg = std::move(arg);
  return a;
}

TEST(HashAggregateOperatorTest, EveryFunctionWithNullInputsAndANullGroup) {
  std::vector<AggregateExpr> aggs;
  aggs.push_back(Agg(AggFunc::kCountStar, nullptr));
  aggs.push_back(Agg(AggFunc::kCount, Col(1)));
  aggs.push_back(Agg(AggFunc::kSum, Col(1)));
  aggs.push_back(Agg(AggFunc::kAvg, Col(1)));
  aggs.push_back(Agg(AggFunc::kMin, Col(1)));
  aggs.push_back(Agg(AggFunc::kMax, Col(1)));
  std::vector<ExprPtr> groups;
  groups.push_back(Col(0));
  auto node_owner = MakeAggregate(std::move(groups), std::move(aggs));
  const PhysicalHashAggregate& node = *node_owner;
  // Groups: 1 -> {4, NULL, 8}; 2 -> {NULL}; NULL -> {5, 7} (two rows with a NULL key, one group).
  auto source = Source(
      {BatchOf({Int64Array({1, 1, 2, 0, 1, 0}, {3, 5}), Int64Array({4, 0, 0, 5, 8, 7}, {1, 2})})});
  EXPECT_EQ(Sorted(RunOp(MakeHashAggregateOperator(node, std::move(source)))),
            (Rows{"1|3|2|12|6|4|8", "2|1|0|NULL|NULL|NULL|NULL", "NULL|2|2|12|6|5|7"}));
}

TEST(HashAggregateOperatorTest, WithoutGroupByAnEmptyInputGivesOneRow) {
  std::vector<AggregateExpr> aggs;
  aggs.push_back(Agg(AggFunc::kCountStar, nullptr));
  aggs.push_back(Agg(AggFunc::kCount, Col(0)));
  aggs.push_back(Agg(AggFunc::kSum, Col(0)));
  aggs.push_back(Agg(AggFunc::kAvg, Col(0)));
  aggs.push_back(Agg(AggFunc::kMin, Col(0)));
  aggs.push_back(Agg(AggFunc::kMax, Col(0)));
  auto node_owner = MakeAggregate({}, std::move(aggs));
  const PhysicalHashAggregate& node = *node_owner;
  EXPECT_EQ(RunOp(MakeHashAggregateOperator(node, Source({}))), (Rows{"0|0|NULL|NULL|NULL|NULL"}));
}

TEST(HashAggregateOperatorTest, GroupByOverAnEmptyInputGivesNoRows) {
  std::vector<AggregateExpr> aggs;
  aggs.push_back(Agg(AggFunc::kCountStar, nullptr));
  std::vector<ExprPtr> groups;
  groups.push_back(Col(0));
  auto node_owner = MakeAggregate(std::move(groups), std::move(aggs));
  const PhysicalHashAggregate& node = *node_owner;
  EXPECT_TRUE(RunOp(MakeHashAggregateOperator(node, Source({}))).empty());
}

TEST(HashAggregateOperatorTest, WithoutGroupByAFilteredAwayInputIsStillOneRow) {
  std::vector<AggregateExpr> aggs;
  aggs.push_back(Agg(AggFunc::kCountStar, nullptr));
  aggs.push_back(Agg(AggFunc::kSum, Col(0)));
  auto node_owner = MakeAggregate({}, std::move(aggs));
  const PhysicalHashAggregate& node = *node_owner;
  PhysicalFilter filter;
  filter.predicate = Bin(BinaryOp::kGt, Col(0), Lit(100));
  auto input = MakeFilterOperator(filter, Source({BatchOf({Int64Array({1, 2, 3})})}));
  EXPECT_EQ(RunOp(MakeHashAggregateOperator(node, std::move(input))), (Rows{"0|NULL"}));
}

TEST(HashAggregateOperatorTest, GroupsAreSplitIntoBatchesAndEvaluatedOverTheLiveRows) {
  std::vector<AggregateExpr> aggs;
  aggs.push_back(Agg(AggFunc::kSum, Bin(BinaryOp::kMul, Col(1), Lit(2))));
  std::vector<ExprPtr> groups;
  groups.push_back(Col(0));
  auto node_owner = MakeAggregate(std::move(groups), std::move(aggs));
  const PhysicalHashAggregate& node = *node_owner;
  PhysicalFilter filter;
  filter.predicate = Bin(BinaryOp::kGt, Col(1), Lit(0));
  std::vector<int64_t> keys;
  std::vector<int64_t> values;
  for (int64_t i = 0; i < 10; ++i) {
    keys.push_back(i);
    values.push_back(i % 2 == 0 ? -1 : i);
  }
  int64_t batches = 0;
  const Rows rows = RunOp(
      MakeHashAggregateOperator(
          node,
          MakeFilterOperator(filter, Source({BatchOf({Int64Array(keys), Int64Array(values)})}))),
      2, &batches);
  EXPECT_EQ(Sorted(rows), (Rows{"1|2", "3|6", "5|10", "7|14", "9|18"}));
  EXPECT_EQ(batches, 3);
}

TEST(HashAggregateOperatorTest, DecimalAndVarcharColumnsAggregate) {
  const LogicalType price = LogicalType::Decimal(15, 2);
  std::vector<AggregateExpr> aggs;
  aggs.push_back(Agg(AggFunc::kSum, Col(1, price)));
  aggs.push_back(Agg(AggFunc::kMax, Col(0, LogicalType::Varchar())));
  std::vector<ExprPtr> groups;
  groups.push_back(Col(0, LogicalType::Varchar()));
  auto node_owner = MakeAggregate(std::move(groups), std::move(aggs));
  const PhysicalHashAggregate& node = *node_owner;
  auto source =
      Source({BatchOf({StringArray({"x", "y", "x"}), Decimal64Array({150, 25, 100}, 15, 2)})});
  EXPECT_EQ(Sorted(RunOp(MakeHashAggregateOperator(node, std::move(source)))),
            (Rows{"x|2.50|x", "y|0.25|y"}));
}

// ---- sort, top-n, limit ----

TEST(SortOperatorTest, MultipleKeysAscendingAndDescending) {
  PhysicalSort node;
  node.output_types = {LogicalType::Int64(), LogicalType::Int64()};
  node.keys.push_back(Key(0, true));
  node.keys.push_back(Key(1, false));
  auto source = Source({BatchOf({Int64Array({2, 1, 2, 1, 2}), Int64Array({1, 5, 9, 7, 4})})});
  EXPECT_EQ(RunOp(MakeSortOperator(node, std::move(source))),
            (Rows{"1|7", "1|5", "2|9", "2|4", "2|1"}));
}

TEST(SortOperatorTest, NullsSortLastForAscendingAndDescendingAndFirstOnRequest) {
  const auto input = [] { return Source({BatchOf({Int64Array({3, 0, 1, 0, 2}, {1, 3})})}); };
  const auto run = [&](bool ascending, bool nulls_first) {
    PhysicalSort node;
    node.output_types = {LogicalType::Int64()};
    node.keys.push_back(Key(0, ascending, nulls_first));
    return RunOp(MakeSortOperator(node, input()));
  };
  EXPECT_EQ(run(true, false), (Rows{"1", "2", "3", "NULL", "NULL"}));
  EXPECT_EQ(run(false, false), (Rows{"3", "2", "1", "NULL", "NULL"}));
  EXPECT_EQ(run(true, true), (Rows{"NULL", "NULL", "1", "2", "3"}));
  EXPECT_EQ(run(false, true), (Rows{"NULL", "NULL", "3", "2", "1"}));
}

TEST(SortOperatorTest, SortsEveryTypeAndKeepsTiesInInputOrder) {
  PhysicalSort node;
  node.output_types = {LogicalType::Varchar(), LogicalType::Double(), LogicalType::Int64()};
  node.keys.push_back(Key(0, true, false, LogicalType::Varchar()));
  auto source = Source(
      {BatchOf({StringArray({"b", "a", "b", "a", "", "c"}),
                DoubleArray({1.5, 2.5, 0.5, -1.0, 9.0, 3.0}), Int64Array({1, 2, 3, 4, 5, 6})})});
  EXPECT_EQ(RunOp(MakeSortOperator(node, std::move(source))),
            (Rows{"|9|5", "a|2.5|2", "a|-1|4", "b|1.5|1", "b|0.5|3", "c|3|6"}));
}

TEST(SortOperatorTest, SortsByAnExpressionAcrossBatchesInBatchSizedPieces) {
  PhysicalSort node;
  node.output_types = {LogicalType::Int64()};
  SortKey key;
  key.expr = Bin(BinaryOp::kMul, Col(0), Lit(-1));  // descending by value through negation
  node.keys.push_back(std::move(key));
  std::vector<Batch> batches;
  for (int64_t b = 0; b < 4; ++b) {
    std::vector<int64_t> values;
    for (int64_t i = 0; i < 5; ++i) values.push_back(b * 5 + i);
    batches.push_back(BatchOf({Int64Array(values)}));
  }
  int64_t out_batches = 0;
  const Rows rows = RunOp(MakeSortOperator(node, Source(std::move(batches))), 8, &out_batches);
  ASSERT_EQ(rows.size(), 20u);
  EXPECT_EQ(rows.front(), "19");
  EXPECT_EQ(rows.back(), "0");
  EXPECT_EQ(out_batches, 3);
}

TEST(SortOperatorTest, DecimalsAndDatesSortByValue) {
  PhysicalSort node;
  node.output_types = {LogicalType::Decimal(15, 2), LogicalType::Date()};
  node.keys.push_back(Key(0, false, false, LogicalType::Decimal(15, 2)));
  node.keys.push_back(Key(1, true, false, LogicalType::Date()));
  auto source = Source(
      {BatchOf({Decimal64Array({100, 250, 100, -5}, 15, 2), testing::DateArray({30, 1, 10, 5})})});
  EXPECT_EQ(RunOp(MakeSortOperator(node, std::move(source))),
            (Rows{"2.50|1", "1.00|10", "1.00|30", "-0.05|5"}));
}

// A pseudo-random input with many ties, in batches of 7.
std::vector<Batch> TieHeavyInput(int rows) {
  std::vector<Batch> batches;
  uint64_t state = 12345;
  for (int start = 0; start < rows; start += 7) {
    std::vector<int64_t> a;
    std::vector<int64_t> b;
    std::vector<int64_t> id;
    testing::NullSet nulls;
    for (int i = start; i < std::min(rows, start + 7); ++i) {
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      a.push_back(static_cast<int64_t>((state >> 33) % 5));
      b.push_back(static_cast<int64_t>((state >> 40) % 3));
      id.push_back(i);
      if ((state >> 50) % 9 == 0) nulls.insert(a.size() - 1);
    }
    batches.push_back(BatchOf({Int64Array(a, nulls), Int64Array(b), Int64Array(id)}));
  }
  return batches;
}

TEST(TopNOperatorTest, EqualsAFullSortThenLimitIncludingTiesAtTheBoundary) {
  for (const int n : {0, 1, 3, 10, 25, 199, 200, 500}) {
    SCOPED_TRACE("n = " + std::to_string(n));
    std::vector<SortKey> keys_for_sort;
    PhysicalSort sort;
    sort.output_types.assign(3, LogicalType::Int64());
    sort.keys.push_back(Key(0, true));
    sort.keys.push_back(Key(1, false));
    PhysicalLimit limit;
    limit.n = n;
    const Rows expected =
        RunOp(MakeLimitOperator(limit, MakeSortOperator(sort, Source(TieHeavyInput(200)))), 16);

    PhysicalTopN top;
    top.output_types.assign(3, LogicalType::Int64());
    top.keys.push_back(Key(0, true));
    top.keys.push_back(Key(1, false));
    top.n = n;
    for (const int32_t batch_size : {1, 4, 2048}) {
      EXPECT_EQ(RunOp(MakeTopNOperator(top, Source(TieHeavyInput(200))), batch_size), expected)
          << "batch size " << batch_size;
    }
  }
}

TEST(TopNOperatorTest, TiesOnEverySortKeyKeepArrivalOrder) {
  PhysicalTopN top;
  top.output_types.assign(2, LogicalType::Int64());
  top.keys.push_back(Key(0, true));
  top.n = 3;
  auto source = Source({BatchOf({Int64Array({1, 1, 1, 1, 0}), Int64Array({10, 11, 12, 13, 14})}),
                        BatchOf({Int64Array({1, 0}), Int64Array({15, 16})})});
  EXPECT_EQ(RunOp(MakeTopNOperator(top, std::move(source)), 2), (Rows{"0|14", "0|16", "1|10"}));
}

TEST(TopNOperatorTest, ZeroReadsNothing) {
  PhysicalTopN top;
  top.output_types.assign(1, LogicalType::Int64());
  top.keys.push_back(Key(0));
  top.n = 0;
  int pulls = 0;
  EXPECT_TRUE(RunOp(MakeTopNOperator(top, Source({BatchOf({Int64Array({1})})}, &pulls))).empty());
  EXPECT_EQ(pulls, 0);
}

TEST(LimitOperatorTest, StopsPullingItsInputOnceSatisfied) {
  PhysicalLimit node;
  node.n = 5;
  int pulls = 0;
  std::vector<Batch> batches;
  for (int64_t b = 0; b < 10; ++b)
    batches.push_back(BatchOf({Int64Array({b * 4, b * 4 + 1, b * 4 + 2, b * 4 + 3})}));
  const Rows rows = RunOp(MakeLimitOperator(node, Source(std::move(batches), &pulls)));
  EXPECT_EQ(rows, (Rows{"0", "1", "2", "3", "4"}));
  EXPECT_EQ(pulls, 2);
}

TEST(LimitOperatorTest, CutsABatchThatHasASelectionAndOneThatDoesNot) {
  PhysicalLimit node;
  node.n = 2;
  PhysicalFilter filter;
  filter.predicate = Bin(BinaryOp::kGt, Col(0), Lit(1));
  EXPECT_EQ(
      RunOp(MakeLimitOperator(
          node, MakeFilterOperator(filter, Source({BatchOf({Int64Array({1, 2, 3, 4, 5})})})))),
      (Rows{"2", "3"}));
  EXPECT_EQ(RunOp(MakeLimitOperator(node, Source({BatchOf({Int64Array({7, 8, 9})})}))),
            (Rows{"7", "8"}));
}

TEST(LimitOperatorTest, ZeroNeverPullsAndALargeLimitPassesEverythingThrough) {
  PhysicalLimit zero;
  zero.n = 0;
  int pulls = 0;
  EXPECT_TRUE(RunOp(MakeLimitOperator(zero, Source({BatchOf({Int64Array({1})})}, &pulls))).empty());
  EXPECT_EQ(pulls, 0);
  PhysicalLimit big;
  big.n = 100;
  EXPECT_EQ(RunOp(MakeLimitOperator(
                big, Source({BatchOf({Int64Array({1, 2})}), BatchOf({Int64Array({3})})}))),
            (Rows{"1", "2", "3"}));
}

// ---- BuildOperators and the profile ----

const Catalog& Events() {
  static const Catalog* catalog = [] {
    auto* c = new Catalog();
    EXPECT_TRUE(c->AddDirectory("tests/data/pruning").ok());
    return c;
  }();
  return *catalog;
}

TEST(BuildOperatorsTest, TheProfileHasOneEntryPerPlanNodeWithRowsAndTimes) {
  auto plan =
      testing::PlanQuery("SELECT id FROM events WHERE id < 10 ORDER BY id DESC LIMIT 3", Events());
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  TestContext context;
  auto op = BuildOperators(**plan, context.context);
  ASSERT_TRUE(op.ok());
  auto batches = Drain(**op, context.context);
  ASSERT_TRUE(batches.ok());
  EXPECT_EQ(RowsOf(*batches), (Rows{"9", "8", "7"}));
  const std::vector<OperatorStats> stats = CollectOperatorStats(**op);
  std::vector<std::string> names;
  for (size_t i = 0; i < stats.size(); ++i) {
    EXPECT_EQ(stats[i].id, static_cast<int>(i));
    EXPECT_GE(stats[i].exclusive_ns, 0);
    names.push_back(stats[i].name);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"TopN", "Project", "Scan"}));
  EXPECT_EQ(stats[0].rows_out, 3);
  EXPECT_EQ(stats[1].rows_out, 10);
  EXPECT_EQ(stats[2].rows_out, 10);  // the scan's predicate already dropped the other rows
  EXPECT_GT(stats[0].peak_state_bytes, 0);
  EXPECT_GT(stats[2].scan_buffer_bytes, 0);
}

TEST(BuildOperatorsTest, RejectsABatchSizeOfZero) {
  auto plan = testing::PlanQuery("SELECT id FROM events", Events());
  ASSERT_TRUE(plan.ok());
  TestContext context(0);
  EXPECT_FALSE(BuildOperators(**plan, context.context).ok());
}

TEST(BuildOperatorsTest, ACancelledQueryEndsAtTheNextPull) {
  auto plan = testing::PlanQuery("SELECT id FROM events", Events());
  ASSERT_TRUE(plan.ok());
  TestContext context(10);
  std::atomic<bool> cancelled{false};
  context.context.cancelled = &cancelled;
  auto op = BuildOperators(**plan, context.context);
  ASSERT_TRUE(op.ok());
  ASSERT_TRUE((*op)->Open(context.context).ok());
  EXPECT_TRUE((*op)->Next().ok());
  cancelled = true;
  auto next = (*op)->Next();
  ASSERT_FALSE(next.ok());
  EXPECT_TRUE(next.status().IsCancelled());
  (*op)->Close();
}

TEST(BuildOperatorsTest, APassedDeadlineIsCancelledAndNamesTheTimeout) {
  auto plan = testing::PlanQuery("SELECT id FROM events", Events());
  ASSERT_TRUE(plan.ok());
  TestContext context;
  context.context.timeout = std::chrono::milliseconds(1);
  context.context.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  auto op = BuildOperators(**plan, context.context);
  ASSERT_TRUE(op.ok());
  auto batches = Drain(**op, context.context);
  ASSERT_FALSE(batches.ok());
  EXPECT_TRUE(batches.status().IsCancelled());
  EXPECT_NE(batches.status().message().find("timeout of 1 ms"), std::string::npos)
      << batches.status().message();
}

}  // namespace
}  // namespace quarry
