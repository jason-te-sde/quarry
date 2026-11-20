#include "vector/selection.h"

#include <arrow/memory_pool.h>
#include <gtest/gtest.h>

#include <vector>

#include "support/value.h"
#include "vector/batch.h"
#include "vector/gather.h"

namespace quarry {
namespace {

using test::DecimalValue;
using test::Value;

TEST(SelectionTest, RejectsUnsortedAndOutOfRange) {
  EXPECT_TRUE(SelectionVector::Make({0, 1, 2}, 3).ok());
  EXPECT_TRUE(SelectionVector::Make({}, 0).ok());
  EXPECT_TRUE(SelectionVector::Make({}, 5).ok());

  const auto descending = SelectionVector::Make({2, 1}, 3);
  ASSERT_FALSE(descending.ok());
  EXPECT_TRUE(descending.status().IsInvalid());
  EXPECT_TRUE(SelectionVector::Make({1, 1}, 3).status().IsInvalid());  // not strictly increasing
  EXPECT_TRUE(SelectionVector::Make({0, 3}, 3).status().IsInvalid());  // equal to the length
  EXPECT_TRUE(SelectionVector::Make({7}, 3).status().IsInvalid());     // beyond the length
  EXPECT_TRUE(SelectionVector::Make({0}, 0).status().IsInvalid());     // nothing to point at
  EXPECT_TRUE(SelectionVector::Make({0}, -1).status().IsInvalid());
}

TEST(SelectionTest, ExposesTheIndicesItWasGiven) {
  const auto selection = SelectionVector::Make({1, 4, 5}, 6).ValueOrDie();
  EXPECT_EQ(selection.size(), 3);
  EXPECT_EQ(selection.batch_length(), 6);
  const auto indices = selection.indices();
  ASSERT_EQ(indices.size(), 3u);
  EXPECT_EQ(indices[0], 1u);
  EXPECT_EQ(indices[2], 5u);
}

Batch MakeBatch(const std::vector<std::vector<Value>>& columns,
                const std::vector<LogicalType>& types, std::optional<SelectionVector> selection) {
  Batch batch;
  for (size_t i = 0; i < columns.size(); ++i) {
    batch.columns.push_back(test::MakeArray(types[i], columns[i]).ValueOrDie());
  }
  batch.length = static_cast<int64_t>(columns.front().size());
  batch.selection = std::move(selection);
  return batch;
}

TEST(BatchTest, ActiveRowsFollowsTheSelection) {
  Batch batch = MakeBatch({{Value(int32_t{1}), Value(int32_t{2}), Value(int32_t{3})}},
                          {LogicalType::Int32()}, std::nullopt);
  EXPECT_EQ(batch.active_rows(), 3);
  batch.selection = SelectionVector::Make({0, 2}, 3).ValueOrDie();
  EXPECT_EQ(batch.active_rows(), 2);
  batch.selection = SelectionVector::Make({}, 3).ValueOrDie();
  EXPECT_EQ(batch.active_rows(), 0);
}

TEST(BatchTest, CompactKeepsTheSelectedRowsInOrder) {
  Batch batch =
      MakeBatch({{Value(int32_t{10}), Value{}, Value(int32_t{30}), Value(int32_t{40})},
                 {Value("a"), Value("bb"), Value{}, Value("dddd")},
                 {Value(DecimalValue{100, 2}), Value(DecimalValue{200, 2}),
                  Value(DecimalValue{300, 2}), Value{}}},
                {LogicalType::Int32(), LogicalType::Varchar(), LogicalType::Decimal(10, 2)},
                SelectionVector::Make({1, 2, 3}, 4).ValueOrDie());
  const Batch compact = batch.Compact(arrow::default_memory_pool()).ValueOrDie();
  EXPECT_FALSE(compact.selection.has_value());
  EXPECT_EQ(compact.length, 3);
  EXPECT_EQ(compact.active_rows(), 3);
  const auto ints = test::ArrayToValues(*compact.columns[0]).ValueOrDie();
  EXPECT_TRUE(test::ValuesEqual(ints[0], Value{}));
  EXPECT_TRUE(test::ValuesEqual(ints[1], Value(int32_t{30})));
  const auto strings = test::ArrayToValues(*compact.columns[1]).ValueOrDie();
  EXPECT_TRUE(test::ValuesEqual(strings[0], Value("bb")));
  EXPECT_TRUE(test::ValuesEqual(strings[1], Value{}));
  EXPECT_TRUE(test::ValuesEqual(strings[2], Value("dddd")));
  const auto decimals = test::ArrayToValues(*compact.columns[2]).ValueOrDie();
  EXPECT_TRUE(test::ValuesEqual(decimals[2], Value{}));
}

TEST(BatchTest, CompactWithoutASelectionSharesTheColumns) {
  Batch batch =
      MakeBatch({{Value(int32_t{1}), Value(int32_t{2})}}, {LogicalType::Int32()}, std::nullopt);
  const Batch compact = batch.Compact(arrow::default_memory_pool()).ValueOrDie();
  EXPECT_EQ(compact.columns[0].get(), batch.columns[0].get());
}

TEST(BatchTest, CompactOfAnEmptySelectionHasZeroRowsOfTheRightTypes) {
  Batch batch = MakeBatch({{Value("x"), Value("y")}, {Value(true), Value(false)}},
                          {LogicalType::Varchar(), LogicalType::Boolean()},
                          SelectionVector::Make({}, 2).ValueOrDie());
  const Batch compact = batch.Compact(arrow::default_memory_pool()).ValueOrDie();
  EXPECT_EQ(compact.length, 0);
  EXPECT_EQ(compact.columns[0]->length(), 0);
  EXPECT_TRUE(compact.columns[0]->type()->Equals(arrow::utf8()));
  EXPECT_TRUE(compact.columns[1]->type()->Equals(arrow::boolean()));
}

TEST(GatherTest, WorksOnSlicedArraysOfEveryType) {
  // An offset array: the gather must index relative to the slice.
  for (const auto& [type, values] : std::vector<std::pair<LogicalType, std::vector<Value>>>{
           {LogicalType::Boolean(), {Value(true), Value(false), Value{}, Value(true)}},
           {LogicalType::Int64(),
            {Value(int64_t{5}), Value{}, Value(int64_t{7}), Value(int64_t{8})}},
           {LogicalType::Double(), {Value(0.5), Value(1.5), Value{}, Value(-2.0)}},
           {LogicalType::Date(),
            {Value(test::DateValue{1}), Value{}, Value(test::DateValue{3}),
             Value(test::DateValue{4})}},
           {LogicalType::Decimal(25, 3),
            {Value(DecimalValue{1, 3}), Value{}, Value(DecimalValue{-3, 3}),
             Value(DecimalValue{4, 3})}},
           {LogicalType::Varchar(), {Value("p"), Value(""), Value{}, Value("sss")}}}) {
    const auto array = test::MakeArray(type, values, /*leading_garbage=*/3).ValueOrDie();
    ASSERT_EQ(array->offset(), 3);
    const std::vector<uint32_t> indices = {3, 0, 2, 2};
    const auto gathered = Gather(*array, indices, arrow::default_memory_pool()).ValueOrDie();
    const auto out = test::ArrayToValues(*gathered).ValueOrDie();
    ASSERT_EQ(out.size(), 4u) << type.ToString();
    for (size_t i = 0; i < indices.size(); ++i) {
      EXPECT_TRUE(test::ValuesEqual(out[i], values[indices[i]]))
          << type.ToString() << " row " << i << ": " << test::ValueToString(out[i]);
    }
  }
}

TEST(GatherTest, OutOfRangeIndexIsInvalid) {
  const auto array = test::MakeArray(LogicalType::Int32(), {Value(int32_t{1})}).ValueOrDie();
  const std::vector<uint32_t> indices = {1};
  EXPECT_TRUE(Gather(*array, indices, arrow::default_memory_pool()).status().IsInvalid());
}

}  // namespace
}  // namespace quarry
