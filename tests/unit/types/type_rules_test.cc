#include "types/type_rules.h"

#include <arrow/type.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>

#include "types/logical_type.h"

namespace quarry {
namespace {

using ::testing::HasSubstr;

LogicalType Dec(int p, int s) { return LogicalType::Decimal(p, s); }
const LogicalType kInt = LogicalType::Int32();
const LogicalType kBig = LogicalType::Int64();
const LogicalType kDbl = LogicalType::Double();

LogicalType Bin(BinaryOp op, const LogicalType& l, const LogicalType& r) {
  return BinaryResultType(op, l, r).ValueOrDie();
}
LogicalType Agg(AggFunc f, const LogicalType& t) { return AggregateResultType(f, t).ValueOrDie(); }

// --- the table of docs/sql.md, row by row ------------------------------------------------------

TEST(TypeRulesTest, DecimalLiterals) {
  EXPECT_EQ(DecimalLiteralType("0.05"), Dec(3, 2));
  EXPECT_EQ(DecimalLiteralType("123.4500"), Dec(7, 4));
  EXPECT_EQ(DecimalLiteralType("1.5"), Dec(2, 1));
  EXPECT_EQ(DecimalLiteralType("100"), Dec(3, 0));
  EXPECT_EQ(DecimalLiteralType(".5"), Dec(1, 1));
  EXPECT_EQ(DecimalLiteralType("0.000"), Dec(4, 3));
  EXPECT_EQ(DecimalLiteralType(std::string(50, '9')), Dec(38, 0));
}

TEST(TypeRulesTest, IntegerLiteralMeetsDecimal) {
  // 1 - 0.05 is DECIMAL(13,2): the INTEGER becomes DECIMAL(10,0).
  EXPECT_EQ(Bin(BinaryOp::kSub, kInt, DecimalLiteralType("0.05")), Dec(13, 2));
}

TEST(TypeRulesTest, DecimalMultiplication) {
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(15, 2), Dec(15, 2)), Dec(18, 4));
  // TPC-H Q1 sum_charge shape: the product of a 128-bit DECIMAL and a DECIMAL(16,2).
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(31, 4), Dec(16, 2)), Dec(38, 6));
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(38, 0), Dec(38, 0)), Dec(38, 0));
  // Both operands fit 64 bits and the width would exceed 18: DuckDB keeps 18 digits.
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(12, 5), Dec(12, 5)), Dec(18, 10));
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(18, 2), Dec(18, 2)), Dec(18, 4));
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(10, 2), Dec(7, 2)), Dec(17, 4));
  // One operand beyond 64 bits: the full width.
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(18, 2), Dec(19, 2)), Dec(37, 4));
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(19, 2), Dec(19, 2)), Dec(38, 4));
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(38, 10), Dec(38, 10)), Dec(38, 20));
  // The width never drops below the scale.
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(18, 18), Dec(18, 18)), Dec(36, 36));
  EXPECT_EQ(Bin(BinaryOp::kMul, Dec(15, 2), kInt), Dec(18, 2));
  EXPECT_TRUE(BinaryResultType(BinaryOp::kMul, Dec(38, 38), Dec(38, 38)).status().IsInvalid());
}

TEST(TypeRulesTest, DecimalAddition) {
  EXPECT_EQ(Bin(BinaryOp::kAdd, Dec(15, 2), kInt), Dec(16, 2));
  EXPECT_EQ(Bin(BinaryOp::kAdd, Dec(15, 2), Dec(10, 4)), Dec(18, 4));
  EXPECT_EQ(Bin(BinaryOp::kSub, Dec(15, 2), Dec(10, 4)), Dec(18, 4));
  EXPECT_EQ(Bin(BinaryOp::kAdd, Dec(15, 2), kBig), Dec(22, 2));
  EXPECT_EQ(Bin(BinaryOp::kAdd, Dec(38, 2), Dec(38, 2)), Dec(38, 2));
  EXPECT_EQ(Bin(BinaryOp::kAdd, Dec(18, 2), Dec(18, 2)), Dec(19, 2));
}

TEST(TypeRulesTest, NumbersOfOtherKinds) {
  EXPECT_EQ(Bin(BinaryOp::kAdd, kInt, kBig), kBig);
  EXPECT_EQ(Bin(BinaryOp::kMul, kInt, kInt), kInt);
  EXPECT_EQ(Bin(BinaryOp::kAdd, kInt, kDbl), kDbl);
  EXPECT_EQ(Bin(BinaryOp::kAdd, Dec(15, 2), kDbl), kDbl);
  EXPECT_EQ(Bin(BinaryOp::kEq, Dec(15, 2), kInt), LogicalType::Boolean());
  EXPECT_EQ(Bin(BinaryOp::kAnd, LogicalType::Boolean(), LogicalType::Boolean()),
            LogicalType::Boolean());
}

TEST(TypeRulesTest, DateArithmeticKeepsTheDate) {
  // DuckDB's DATE + INTERVAL is a TIMESTAMP; quarry folds the interval into a DATE (docs/sql.md).
  EXPECT_EQ(Bin(BinaryOp::kAdd, LogicalType::Date(), kInt), LogicalType::Date());
  EXPECT_EQ(Bin(BinaryOp::kAdd, kInt, LogicalType::Date()), LogicalType::Date());
  EXPECT_EQ(Bin(BinaryOp::kSub, LogicalType::Date(), kInt), LogicalType::Date());
  EXPECT_TRUE(BinaryResultType(BinaryOp::kSub, kInt, LogicalType::Date()).status().IsInvalid());
  EXPECT_TRUE(BinaryResultType(BinaryOp::kMul, LogicalType::Date(), kInt).status().IsInvalid());
  EXPECT_TRUE(BinaryResultType(BinaryOp::kAdd, LogicalType::Date(), kBig).status().IsInvalid());
}

TEST(TypeRulesTest, Aggregates) {
  EXPECT_EQ(Agg(AggFunc::kSum, Dec(15, 2)), Dec(38, 2));
  // The one intentional difference from DuckDB, which says HUGEINT.
  EXPECT_EQ(Agg(AggFunc::kSum, kInt), Dec(38, 0));
  EXPECT_EQ(Agg(AggFunc::kSum, kBig), Dec(38, 0));
  EXPECT_EQ(Agg(AggFunc::kSum, kDbl), kDbl);
  EXPECT_EQ(Agg(AggFunc::kAvg, Dec(15, 2)), kDbl);
  EXPECT_EQ(Agg(AggFunc::kAvg, kInt), kDbl);
  EXPECT_EQ(Agg(AggFunc::kMin, Dec(15, 2)), Dec(15, 2));
  EXPECT_EQ(Agg(AggFunc::kMax, Dec(15, 2)), Dec(15, 2));
  EXPECT_EQ(Agg(AggFunc::kMin, LogicalType::Date()), LogicalType::Date());
  EXPECT_EQ(Agg(AggFunc::kMax, LogicalType::Varchar()), LogicalType::Varchar());
  EXPECT_EQ(Agg(AggFunc::kCountStar, LogicalType::Boolean()), kBig);
  EXPECT_EQ(Agg(AggFunc::kCount, LogicalType::Varchar()), kBig);
  EXPECT_TRUE(AggregateResultType(AggFunc::kSum, LogicalType::Varchar()).status().IsInvalid());
  EXPECT_TRUE(AggregateResultType(AggFunc::kAvg, LogicalType::Date()).status().IsInvalid());
}

TEST(TypeRulesTest, CommonComparisonTypes) {
  EXPECT_EQ(CommonComparisonType(Dec(15, 2), Dec(2, 1)).ValueOrDie(), Dec(15, 2));
  EXPECT_EQ(CommonComparisonType(Dec(5, 4), Dec(15, 2)).ValueOrDie(), Dec(17, 4));
  EXPECT_EQ(CommonComparisonType(Dec(15, 2), kInt).ValueOrDie(), Dec(15, 2));
  EXPECT_EQ(CommonComparisonType(kInt, kBig).ValueOrDie(), kBig);
  EXPECT_EQ(CommonComparisonType(kBig, kDbl).ValueOrDie(), kDbl);
  EXPECT_EQ(CommonComparisonType(Dec(10, 2), kDbl).ValueOrDie(), kDbl);
  EXPECT_EQ(CommonComparisonType(LogicalType::Date(), LogicalType::Date()).ValueOrDie(),
            LogicalType::Date());
  EXPECT_EQ(CommonComparisonType(Dec(38, 0), Dec(38, 10)).ValueOrDie(), Dec(38, 10));
}

TEST(TypeRulesTest, InvalidCombinationsSayWhy) {
  const auto bad = BinaryResultType(BinaryOp::kAdd, LogicalType::Varchar(), kInt);
  ASSERT_TRUE(bad.status().IsInvalid());
  EXPECT_THAT(bad.status().message(), HasSubstr("VARCHAR"));
  EXPECT_THAT(bad.status().message(), HasSubstr("INTEGER"));
  EXPECT_TRUE(BinaryResultType(BinaryOp::kAnd, kInt, kInt).status().IsInvalid());
  EXPECT_TRUE(BinaryResultType(BinaryOp::kEq, LogicalType::Varchar(), kInt).status().IsInvalid());
  EXPECT_TRUE(BinaryResultType(BinaryOp::kLt, LogicalType::Date(), kInt).status().IsInvalid());
  EXPECT_TRUE(BinaryResultType(BinaryOp::kEq, LogicalType::Boolean(), kInt).status().IsInvalid());
}

// Every row of the table scripts/duckdb_types.py wrote into docs/sql.md is checked against the
// rules, so that the document, the DuckDB it was generated from and the code cannot drift apart.
TEST(TypeRulesTest, DocsTableMatchesTheRules) {
  std::ifstream file("docs/sql.md");
  ASSERT_TRUE(file.good()) << "docs/sql.md must be readable from the repository root";
  const std::map<std::string, std::function<LogicalType()>> rules = {
      {"0.05", [] { return DecimalLiteralType("0.05"); }},
      {"1", [] { return kInt; }},
      {"1 - 0.05", [] { return Bin(BinaryOp::kSub, kInt, DecimalLiteralType("0.05")); }},
      {"DECIMAL(15,2) * DECIMAL(15,2)", [] { return Bin(BinaryOp::kMul, Dec(15, 2), Dec(15, 2)); }},
      {"DECIMAL(31,4) * DECIMAL(16,2)", [] { return Bin(BinaryOp::kMul, Dec(31, 4), Dec(16, 2)); }},
      {"DECIMAL(15,2) + INTEGER", [] { return Bin(BinaryOp::kAdd, Dec(15, 2), kInt); }},
      {"DECIMAL(15,2) + DECIMAL(10,4)", [] { return Bin(BinaryOp::kAdd, Dec(15, 2), Dec(10, 4)); }},
      {"DECIMAL(15,2) - DECIMAL(10,4)", [] { return Bin(BinaryOp::kSub, Dec(15, 2), Dec(10, 4)); }},
      {"DECIMAL(38,0) * DECIMAL(38,0)", [] { return Bin(BinaryOp::kMul, Dec(38, 0), Dec(38, 0)); }},
      {"DECIMAL(12,5) * DECIMAL(12,5)", [] { return Bin(BinaryOp::kMul, Dec(12, 5), Dec(12, 5)); }},
      {"DECIMAL(18,2) * DECIMAL(19,2)", [] { return Bin(BinaryOp::kMul, Dec(18, 2), Dec(19, 2)); }},
      {"DECIMAL(18,18) * DECIMAL(18,18)",
       [] { return Bin(BinaryOp::kMul, Dec(18, 18), Dec(18, 18)); }},
      {"DECIMAL(38,10) * DECIMAL(38,10)",
       [] { return Bin(BinaryOp::kMul, Dec(38, 10), Dec(38, 10)); }},
      {"DECIMAL(38,2) + DECIMAL(38,2)", [] { return Bin(BinaryOp::kAdd, Dec(38, 2), Dec(38, 2)); }},
      {"DECIMAL(15,2) * INTEGER", [] { return Bin(BinaryOp::kMul, Dec(15, 2), kInt); }},
      {"DECIMAL(15,2) + BIGINT", [] { return Bin(BinaryOp::kAdd, Dec(15, 2), kBig); }},
      {"SUM(DECIMAL(15,2))", [] { return Agg(AggFunc::kSum, Dec(15, 2)); }},
      {"SUM(INTEGER)", [] { return Agg(AggFunc::kSum, kInt); }},
      {"SUM(BIGINT)", [] { return Agg(AggFunc::kSum, kBig); }},
      {"AVG(DECIMAL(15,2))", [] { return Agg(AggFunc::kAvg, Dec(15, 2)); }},
      {"AVG(INTEGER)", [] { return Agg(AggFunc::kAvg, kInt); }},
      {"MIN(DECIMAL(15,2))", [] { return Agg(AggFunc::kMin, Dec(15, 2)); }},
      {"MAX(DECIMAL(15,2))", [] { return Agg(AggFunc::kMax, Dec(15, 2)); }},
      {"COUNT(*)", [] { return Agg(AggFunc::kCountStar, kInt); }},
      {"DATE + INTERVAL 1 MONTH", [] { return Bin(BinaryOp::kAdd, LogicalType::Date(), kInt); }},
      {"INTEGER + BIGINT", [] { return Bin(BinaryOp::kAdd, kInt, kBig); }},
      {"INTEGER * INTEGER", [] { return Bin(BinaryOp::kMul, kInt, kInt); }},
      {"INTEGER + DOUBLE", [] { return Bin(BinaryOp::kAdd, kInt, kDbl); }},
      {"DECIMAL(15,2) + DOUBLE", [] { return Bin(BinaryOp::kAdd, Dec(15, 2), kDbl); }},
      {"DECIMAL(15,2) = INTEGER", [] { return Bin(BinaryOp::kEq, Dec(15, 2), kInt); }},
  };
  // Where quarry deliberately differs from DuckDB's typeof: it has no HUGEINT and no TIMESTAMP.
  const std::map<std::string, std::string> intentional = {
      {"SUM(INTEGER)", "DECIMAL(38,0)"},
      {"SUM(BIGINT)", "DECIMAL(38,0)"},
      {"DATE + INTERVAL 1 MONTH", "DATE"},
  };

  std::string line;
  bool in_table = false;
  int checked = 0;
  while (std::getline(file, line)) {
    if (line == "<!-- duckdb-types -->") in_table = true;
    if (line == "<!-- /duckdb-types -->") break;
    // A type row: | `expression` | `typeof` |, before the probes table.
    if (!in_table || line.rfind("| `", 0) != 0) continue;
    const size_t mid = line.find("` | `");
    if (mid == std::string::npos || line.find("| Probe") != std::string::npos) continue;
    const std::string label = line.substr(3, mid - 3);
    const std::string typeof_text = line.substr(mid + 5, line.size() - mid - 5 - 3);
    const auto rule = rules.find(label);
    if (rule == rules.end()) {
      // The probes table (ORDER BY, CAST, DATE results) has other kinds of rows.
      continue;
    }
    const auto diff = intentional.find(label);
    const std::string expected = diff == intentional.end() ? typeof_text : diff->second;
    EXPECT_EQ(rule->second().ToString(), expected) << "row: " << label;
    ++checked;
  }
  EXPECT_EQ(checked, static_cast<int>(rules.size()))
      << "a row of the rules table is missing from docs/sql.md";
}

// --- the type itself -----------------------------------------------------------------------------

TEST(LogicalTypeTest, ToArrowAndBack) {
  EXPECT_TRUE(Dec(15, 2).ToArrow()->Equals(arrow::decimal64(15, 2)));
  EXPECT_TRUE(Dec(18, 0).ToArrow()->Equals(arrow::decimal64(18, 0)));
  EXPECT_TRUE(Dec(19, 2).ToArrow()->Equals(arrow::decimal128(19, 2)));
  EXPECT_TRUE(Dec(38, 6).ToArrow()->Equals(arrow::decimal128(38, 6)));
  EXPECT_TRUE(LogicalType::Date().ToArrow()->Equals(arrow::date32()));
  EXPECT_TRUE(LogicalType::Varchar().ToArrow()->Equals(arrow::utf8()));
  for (const LogicalType& type :
       {LogicalType::Boolean(), kInt, kBig, kDbl, LogicalType::Date(), LogicalType::Varchar(),
        Dec(1, 0), Dec(15, 2), Dec(18, 18), Dec(19, 0), Dec(38, 38)}) {
    EXPECT_EQ(LogicalType::FromArrow(*type.ToArrow()).ValueOrDie(), type) << type.ToString();
  }
}

TEST(LogicalTypeTest, ParquetDecimal128WithSmallPrecisionIsAccepted) {
  // The Parquet reader returns decimal128 even for DECIMAL(15,2).
  EXPECT_EQ(LogicalType::FromArrow(*arrow::decimal128(15, 2)).ValueOrDie(), Dec(15, 2));
  EXPECT_EQ(LogicalType::FromArrow(*arrow::decimal128(18, 4)).ValueOrDie(), Dec(18, 4));
  EXPECT_EQ(LogicalType::FromArrow(*arrow::decimal64(10, 2)).ValueOrDie(), Dec(10, 2));
  EXPECT_EQ(LogicalType::FromArrow(*arrow::decimal128(38, 2)).ValueOrDie(), Dec(38, 2));
}

TEST(LogicalTypeTest, UnsupportedArrowTypesAreInvalid) {
  for (const auto& type : std::vector<std::shared_ptr<arrow::DataType>>{
           arrow::float32(), arrow::uint64(), arrow::int16(),
           arrow::timestamp(arrow::TimeUnit::MICRO), arrow::list(arrow::int32()),
           arrow::decimal256(40, 2), arrow::large_utf8(), arrow::date64(), arrow::binary(),
           arrow::null()}) {
    const auto result = LogicalType::FromArrow(*type);
    ASSERT_TRUE(result.status().IsInvalid()) << type->ToString();
    EXPECT_THAT(result.status().message(), HasSubstr(type->ToString()));
  }
}

TEST(LogicalTypeTest, Names) {
  EXPECT_EQ(Dec(15, 2).ToString(), "DECIMAL(15,2)");
  EXPECT_EQ(kInt.ToString(), "INTEGER");
  EXPECT_EQ(kBig.ToString(), "BIGINT");
  EXPECT_EQ(kDbl.ToString(), "DOUBLE");
  EXPECT_EQ(LogicalType::Date().ToString(), "DATE");
  EXPECT_EQ(LogicalType::Varchar().ToString(), "VARCHAR");
  EXPECT_EQ(LogicalType::Boolean().ToString(), "BOOLEAN");
}

}  // namespace
}  // namespace quarry
