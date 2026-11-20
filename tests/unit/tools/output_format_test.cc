#include "output_format.h"

#include <arrow/api.h>
#include <gtest/gtest.h>

#include <limits>
#include <string>

#include "support/arrays.h"

namespace quarry {
namespace {

using testing::BoolArray;
using testing::DateArray;
using testing::Decimal128Array;
using testing::Decimal64Array;
using testing::DoubleArray;
using testing::Int32Array;
using testing::Int64Array;
using testing::StringArray;

std::shared_ptr<arrow::Table> MakeTable(
    const std::vector<std::pair<std::string, std::shared_ptr<arrow::Array>>>& columns) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  for (const auto& [name, array] : columns) {
    fields.push_back(arrow::field(name, array->type()));
    arrays.push_back(array);
  }
  return arrow::Table::Make(arrow::schema(fields), arrays);
}

TEST(OutputFormatTest, JsonOfEveryType) {
  const auto table = MakeTable({
      {"i", Int32Array({1, -2, 3}, {2})},
      {"b", Int64Array({10000000000, 0, -5}, {1})},
      {"d", Decimal64Array({150, -5, 0}, 12, 2, {2})},
      {"w", Decimal128Array({12345, -1, 7}, 38, 4, {1})},
      {"x", DoubleArray({0.1, 3.0, 1e22}, {})},
      {"t", DateArray({0, 18262, -1}, {2})},
      {"s", StringArray({"a", "b\"c", "d"}, {2})},
      {"f", BoolArray({true, false, true}, {1})},
  });
  EXPECT_EQ(
      ToJson(*table),
      "{\"columns\":[{\"name\":\"i\",\"type\":\"INTEGER\"},{\"name\":\"b\",\"type\":\"BIGINT\"},"
      "{\"name\":\"d\",\"type\":\"DECIMAL(12,2)\"},{\"name\":\"w\",\"type\":\"DECIMAL(38,4)\"},"
      "{\"name\":\"x\",\"type\":\"DOUBLE\"},{\"name\":\"t\",\"type\":\"DATE\"},"
      "{\"name\":\"s\",\"type\":\"VARCHAR\"},{\"name\":\"f\",\"type\":\"BOOLEAN\"}],\"rows\":["
      "[1,10000000000,\"1.50\",\"1.2345\",0.10000000000000001,\"1970-01-01\",\"a\",true],"
      "[-2,null,\"-0.05\",null,3,\"2020-01-01\",\"b\\\"c\",null],"
      "[null,-5,null,\"0.0007\",1e+22,null,null,true]]}");
}

TEST(OutputFormatTest, JsonStringsAreEscapedLikePythonsJsonDumpsWithoutAscii) {
  const auto table =
      MakeTable({{"s", StringArray({"a\\b", "line\nbreak\ttab\r", std::string("\x01\x1f", 2),
                                    "caf\xc3\xa9", "\x7f"})}});
  EXPECT_EQ(ToJson(*table),
            "{\"columns\":[{\"name\":\"s\",\"type\":\"VARCHAR\"}],\"rows\":[[\"a\\\\b\"],"
            "[\"line\\nbreak\\ttab\\r\"],[\"\\u0001\\u001f\"],[\"caf\xc3\xa9\"],[\"\x7f\"]]}");
}

TEST(OutputFormatTest, ANonFiniteDoubleIsNull) {
  const auto table =
      MakeTable({{"x", DoubleArray({std::numeric_limits<double>::infinity(),
                                    std::numeric_limits<double>::quiet_NaN(), -0.0})}});
  EXPECT_EQ(ToJson(*table),
            "{\"columns\":[{\"name\":\"x\",\"type\":\"DOUBLE\"}],\"rows\":[[null],[null],[-0]]}");
}

TEST(OutputFormatTest, AnEmptyResultHasColumnsAndNoRows) {
  const auto table = MakeTable({{"x", Int64Array({})}});
  EXPECT_EQ(ToJson(*table), "{\"columns\":[{\"name\":\"x\",\"type\":\"BIGINT\"}],\"rows\":[]}");
  EXPECT_EQ(ToCsv(*table), "x\n");
  EXPECT_EQ(ToTable(*table), "x\n-\n(0 rows)\n");
}

TEST(OutputFormatTest, ATableWithNoChunksIsEmpty) {
  const auto schema = arrow::schema({arrow::field("x", arrow::utf8())});
  std::vector<std::shared_ptr<arrow::ChunkedArray>> columns = {
      std::make_shared<arrow::ChunkedArray>(arrow::ArrayVector{}, arrow::utf8())};
  const auto table = arrow::Table::Make(schema, columns);
  EXPECT_EQ(ToJson(*table), "{\"columns\":[{\"name\":\"x\",\"type\":\"VARCHAR\"}],\"rows\":[]}");
}

TEST(OutputFormatTest, ChunkedColumnsAreOneColumn) {
  const auto schema = arrow::schema({arrow::field("x", arrow::int64())});
  std::vector<std::shared_ptr<arrow::ChunkedArray>> columns = {
      std::make_shared<arrow::ChunkedArray>(
          arrow::ArrayVector{Int64Array({1, 2}), Int64Array({3})})};
  const auto table = arrow::Table::Make(schema, columns);
  EXPECT_EQ(ToJson(*table),
            "{\"columns\":[{\"name\":\"x\",\"type\":\"BIGINT\"}],\"rows\":[[1],[2],[3]]}");
}

TEST(OutputFormatTest, CsvQuotesWhatNeedsIt) {
  const auto table =
      MakeTable({{"a,b", StringArray({"x,y", "say \"hi\"", "plain", "two\nlines", ""}, {4})},
                 {"n", Decimal64Array({150, 5, 0, 1, 2}, 10, 2, {1})}});
  EXPECT_EQ(
      ToCsv(*table),
      "\"a,b\",n\n\"x,y\",1.50\n\"say \"\"hi\"\"\",\nplain,0.00\n\"two\nlines\",0.01\n,0.02\n");
}

TEST(OutputFormatTest, TheTableIsAlignedWithNumbersOnTheRight) {
  const auto table = MakeTable({{"name", StringArray({"ab", "c"}, {})},
                                {"n", Int64Array({5, 1000})},
                                {"x", DoubleArray({0.5, 0.0}, {1})}});
  EXPECT_EQ(ToTable(*table),
            "name |    n |    x\n"
            "-----+------+-----\n"
            "ab   |    5 |  0.5\n"
            "c    | 1000 | NULL\n"
            "(2 rows)\n");
}

TEST(OutputFormatTest, OneRowSaysRowNotRows) {
  EXPECT_EQ(ToTable(*MakeTable({{"x", Int64Array({1})}})), "x\n-\n1\n(1 row)\n");
}

}  // namespace
}  // namespace quarry
