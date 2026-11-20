#include "exec/agg_hash_table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "support/arrays.h"

namespace quarry {
namespace {

using testing::Cell;
using testing::Cells;
using testing::Decimal128Array;
using testing::Decimal64Array;
using testing::DoubleArray;
using testing::Int32Array;
using testing::Int64Array;
using testing::StringArray;

AggSpec CountStar() { return {AggFunc::kCountStar, LogicalType::Int64(), LogicalType::Int64()}; }

AggSpec Spec(AggFunc func, const LogicalType& arg) {
  auto result = AggregateResultType(func, arg);
  EXPECT_TRUE(result.ok()) << result.status().ToString();
  return {func, arg, *result};
}

// Finishes the table into rows of text: "key|agg|agg", sorted, so tests do not depend on group
// order.
std::vector<std::string> Rows(const AggHashTable& table) {
  auto columns = table.Finish(0, table.num_groups(), arrow::default_memory_pool());
  EXPECT_TRUE(columns.ok()) << columns.status().ToString();
  std::vector<std::string> rows;
  if (!columns.ok()) return rows;
  for (int64_t i = 0; i < table.num_groups(); ++i) {
    std::string row;
    for (size_t c = 0; c < columns->size(); ++c) {
      row += (c == 0 ? "" : "|") + Cell(*(*columns)[c], i);
    }
    rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end());
  return rows;
}

TEST(AggHashTableTest, ConstantHashStillMatchesByKey) {
  AggHashTable table({LogicalType::Int64()},
                     {CountStar(), Spec(AggFunc::kSum, LogicalType::Int64())}, nullptr,
                     [](std::span<const std::byte>) -> uint64_t { return 42; });
  std::vector<int64_t> keys;
  std::vector<int64_t> values;
  for (int round = 0; round < 3; ++round) {
    for (int64_t k = 0; k < 1000; ++k) {
      keys.push_back(k);
      values.push_back(k + round);
    }
  }
  ASSERT_TRUE(table.Update({Int64Array(keys)}, {nullptr, Int64Array(values)}, 3000).ok());
  EXPECT_EQ(table.num_groups(), 1000);
  auto columns = table.Finish(0, 1000, arrow::default_memory_pool());
  ASSERT_TRUE(columns.ok());
  for (int64_t g = 0; g < 1000; ++g) {
    const int64_t key = std::stoll(Cell(*(*columns)[0], g));
    EXPECT_EQ(Cell(*(*columns)[1], g), "3") << "key " << key;
    EXPECT_EQ(Cell(*(*columns)[2], g), std::to_string(3 * key + 3)) << "key " << key;
  }
}

TEST(AggHashTableTest, DuplicateKeysAccumulateAcrossBatches) {
  AggHashTable table({LogicalType::Int64()},
                     {CountStar(), Spec(AggFunc::kSum, LogicalType::Int64())});
  ASSERT_TRUE(table.Update({Int64Array({1, 2, 1})}, {nullptr, Int64Array({10, 20, 30})}, 3).ok());
  ASSERT_TRUE(table.Update({Int64Array({2, 1})}, {nullptr, Int64Array({5, 1})}, 2).ok());
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"1|3|41", "2|2|25"}));
}

TEST(AggHashTableTest, GroupsAreNumberedInOrderOfFirstAppearance) {
  AggHashTable table({LogicalType::Varchar()}, {CountStar()});
  ASSERT_TRUE(table.Update({StringArray({"b", "a", "b", "c"})}, {nullptr}, 4).ok());
  auto columns = table.Finish(0, 3, arrow::default_memory_pool());
  ASSERT_TRUE(columns.ok());
  EXPECT_EQ(Cells(*(*columns)[0]), (std::vector<std::string>{"b", "a", "c"}));
  auto tail = table.Finish(1, 2, arrow::default_memory_pool());
  ASSERT_TRUE(tail.ok());
  EXPECT_EQ(Cells(*(*tail)[0]), (std::vector<std::string>{"a", "c"}));
  EXPECT_EQ(Cells(*(*tail)[1]), (std::vector<std::string>{"1", "1"}));
}

TEST(AggHashTableTest, NullKeysFormOneGroup) {
  AggHashTable table({LogicalType::Int64()}, {CountStar()});
  ASSERT_TRUE(table.Update({Int64Array({1, 0, 0, 1, 0}, {1, 2, 4})}, {nullptr}, 5).ok());
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"1|2", "NULL|3"}));
}

TEST(AggHashTableTest, MultiColumnKeys) {
  AggHashTable table({LogicalType::Int64(), LogicalType::Varchar()},
                     {Spec(AggFunc::kMax, LogicalType::Int64())});
  ASSERT_TRUE(table
                  .Update({Int64Array({1, 1, 2, 1}), StringArray({"a", "b", "a", "a"})},
                          {Int64Array({5, 6, 7, 9})}, 4)
                  .ok());
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"1|a|9", "1|b|6", "2|a|7"}));
}

TEST(AggHashTableTest, GrowsPastInitialCapacity) {
  AggHashTable table({LogicalType::Int64()}, {CountStar()});
  for (int64_t start = 0; start < 60000; start += 4000) {
    std::vector<int64_t> keys;
    for (int64_t i = start; i < start + 4000; ++i) keys.push_back(i % 25000);
    ASSERT_TRUE(table.Update({Int64Array(keys)}, {nullptr}, 4000).ok());
  }
  EXPECT_EQ(table.num_groups(), 25000);
  auto columns = table.Finish(0, 25000, arrow::default_memory_pool());
  ASSERT_TRUE(columns.ok());
  int64_t total = 0;
  for (int64_t g = 0; g < 25000; ++g) total += std::stoll(Cell(*(*columns)[1], g));
  EXPECT_EQ(total, 60000);
  EXPECT_GT(table.memory_bytes(), 0);
}

TEST(AggHashTableTest, EveryAggregateFunctionOverIntegers) {
  AggHashTable table(
      {LogicalType::Int64()},
      {Spec(AggFunc::kSum, LogicalType::Int32()), Spec(AggFunc::kAvg, LogicalType::Int32()),
       Spec(AggFunc::kCount, LogicalType::Int32()), CountStar(),
       Spec(AggFunc::kMin, LogicalType::Int32()), Spec(AggFunc::kMax, LogicalType::Int32())});
  const auto values = Int32Array({4, 0, 6, 9, 0}, {1, 4});
  ASSERT_TRUE(table
                  .Update({Int64Array({1, 1, 1, 2, 2})},
                          {values, values, values, nullptr, values, values}, 5)
                  .ok());
  // Group 1: 4, NULL, 6. Group 2: 9, NULL.
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"1|10|5|2|3|4|6", "2|9|9|1|2|9|9"}));
}

TEST(AggHashTableTest, AGroupOfNullsHasNullSumAvgMinMaxAndZeroCount) {
  AggHashTable table(
      {LogicalType::Int64()},
      {Spec(AggFunc::kSum, LogicalType::Int64()), Spec(AggFunc::kAvg, LogicalType::Int64()),
       Spec(AggFunc::kCount, LogicalType::Int64()), Spec(AggFunc::kMin, LogicalType::Int64()),
       Spec(AggFunc::kMax, LogicalType::Int64())});
  const auto nulls = Int64Array({0, 0}, {0, 1});
  ASSERT_TRUE(table.Update({Int64Array({7, 7})}, {nulls, nulls, nulls, nulls, nulls}, 2).ok());
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"7|NULL|NULL|0|NULL|NULL"}));
}

TEST(AggHashTableTest, DecimalSumKeepsTheScaleAndWidens) {
  const LogicalType price = LogicalType::Decimal(15, 2);
  AggHashTable table({}, {Spec(AggFunc::kSum, price), Spec(AggFunc::kAvg, price),
                          Spec(AggFunc::kMin, price), Spec(AggFunc::kMax, price)});
  const auto values = Decimal64Array({1050, 250, -75}, 15, 2);
  ASSERT_TRUE(table.Update({}, {values, values, values, values}, 3).ok());
  auto columns = table.Finish(0, 1, arrow::default_memory_pool());
  ASSERT_TRUE(columns.ok());
  ASSERT_EQ(columns->size(), 4u);
  EXPECT_EQ((*columns)[0]->type()->ToString(), "decimal128(38, 2)");
  EXPECT_EQ(Cell(*(*columns)[0], 0), "12.25");
  EXPECT_EQ(Cell(*(*columns)[1], 0), "4.083333333333333");  // 12.25 / 3 as a double
  EXPECT_EQ(Cell(*(*columns)[2], 0), "-0.75");
  EXPECT_EQ(Cell(*(*columns)[3], 0), "10.50");
}

TEST(AggHashTableTest, WideDecimalSumsAndOverflow) {
  const LogicalType wide = LogicalType::Decimal(38, 0);
  AggHashTable table({}, {Spec(AggFunc::kSum, wide)});
  const Int128 big = PowerOfTen<Int128>(37) * 9;
  ASSERT_TRUE(table.Update({}, {Decimal128Array({big}, 38, 0)}, 1).ok());
  const arrow::Status overflow = table.Update({}, {Decimal128Array({big}, 38, 0)}, 1);
  ASSERT_FALSE(overflow.ok());
  EXPECT_NE(overflow.message().find("overflow"), std::string::npos) << overflow.ToString();
}

TEST(AggHashTableTest, DoubleAndStringAggregates) {
  AggHashTable table(
      {},
      {Spec(AggFunc::kSum, LogicalType::Double()), Spec(AggFunc::kAvg, LogicalType::Double()),
       Spec(AggFunc::kMin, LogicalType::Varchar()), Spec(AggFunc::kMax, LogicalType::Varchar())});
  ASSERT_TRUE(table
                  .Update({},
                          {DoubleArray({1.5, 2.5, 4.0}), DoubleArray({1.5, 2.5, 4.0}),
                           StringArray({"pear", "apple", "zebra"}),
                           StringArray({"pear", "apple", "zebra"})},
                          3)
                  .ok());
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"8|2.6666666666666665|apple|zebra"}));
}

TEST(AggHashTableTest, MinMaxOfDatesAndBigIntegers) {
  AggHashTable table(
      {}, {Spec(AggFunc::kMin, LogicalType::Date()), Spec(AggFunc::kMax, LogicalType::Date()),
           Spec(AggFunc::kMin, LogicalType::Int64()), Spec(AggFunc::kMax, LogicalType::Int64())});
  const auto dates = testing::DateArray({18000, 17000, 19000});
  const auto longs = Int64Array({-9000000000000, 5, 9000000000000});
  ASSERT_TRUE(table.Update({}, {dates, dates, longs, longs}, 3).ok());
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"17000|19000|-9000000000000|9000000000000"}));
}

TEST(AggHashTableTest, WithoutKeysAnEmptyInputHasNoGroupUntilEnsured) {
  AggHashTable table({}, {CountStar(), Spec(AggFunc::kSum, LogicalType::Int64())});
  EXPECT_EQ(table.num_groups(), 0);
  ASSERT_TRUE(table.Update({}, {nullptr, Int64Array({})}, 0).ok());
  ASSERT_TRUE(table.EnsureGlobalGroup().ok());
  ASSERT_TRUE(table.EnsureGlobalGroup().ok());  // idempotent
  EXPECT_EQ(Rows(table), (std::vector<std::string>{"0|NULL"}));
}

TEST(AggHashTableTest, MemoryIsChargedToTheReservationAndRefusedOverTheLimit) {
  MemoryBudget budget(20000);
  Reservation reservation(budget, "HashAggregate #1");
  AggHashTable table({LogicalType::Int64()}, {CountStar()}, &reservation);
  std::vector<int64_t> keys;
  for (int64_t i = 0; i < 100; ++i) keys.push_back(i);
  ASSERT_TRUE(table.Update({Int64Array(keys)}, {nullptr}, 100).ok());
  EXPECT_GT(reservation.size(), 0);
  EXPECT_EQ(reservation.size(), budget.used());

  keys.clear();
  for (int64_t i = 0; i < 20000; ++i) keys.push_back(1000 + i);
  const arrow::Status status = table.Update({Int64Array(keys)}, {nullptr}, 20000);
  ASSERT_TRUE(status.IsOutOfMemory()) << status.ToString();
  EXPECT_NE(status.message().find("resource exhausted: HashAggregate #1 needs"), std::string::npos)
      << status.message();
}

}  // namespace
}  // namespace quarry
