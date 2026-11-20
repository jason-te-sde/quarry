#include "expr/eval.h"

#include <arrow/array/builder_decimal.h>
#include <arrow/memory_pool.h>
#include <arrow/scalar.h>
#include <arrow/util/decimal.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "expr/bound_expr.h"
#include "support/value.h"
#include "types/date.h"

namespace quarry {
namespace {

using test::DateValue;
using test::DecimalValue;
using test::Value;
using ::testing::HasSubstr;

const LogicalType kBool = LogicalType::Boolean();
const LogicalType kInt = LogicalType::Int32();
const LogicalType kBig = LogicalType::Int64();
const LogicalType kDbl = LogicalType::Double();
const LogicalType kDate = LogicalType::Date();
const LogicalType kStr = LogicalType::Varchar();
LogicalType Dec(int p, int s) { return LogicalType::Decimal(p, s); }

const Value kNull{};
Value I(int32_t v) { return Value(v); }
Value L(int64_t v) { return Value(v); }
Value D(Int128 raw, int scale) { return Value(DecimalValue{raw, scale}); }
Value S(const char* s) { return Value(std::string(s)); }
Value T() { return Value(true); }
Value F() { return Value(false); }

ExprPtr Must(arrow::Result<ExprPtr> expr) { return std::move(expr).ValueOrDie(); }
ExprPtr Col(int index, const LogicalType& type) { return MakeColumn(index, type); }
ExprPtr Lit(std::shared_ptr<arrow::Scalar> scalar) { return Must(MakeLiteral(std::move(scalar))); }
ExprPtr IntLit(int32_t v) { return Lit(std::make_shared<arrow::Int32Scalar>(v)); }
ExprPtr DecLit(const char* digits) { return Must(MakeDecimalLiteral(digits)); }
ExprPtr Bin(BinaryOp op, ExprPtr l, ExprPtr r) {
  return Must(MakeBinary(op, std::move(l), std::move(r)));
}

struct Columns {
  std::vector<LogicalType> types;
  std::vector<std::vector<Value>> values;
};

Batch MakeBatch(const Columns& columns,
                std::optional<std::vector<uint32_t>> selection = std::nullopt) {
  Batch batch;
  for (size_t i = 0; i < columns.types.size(); ++i) {
    batch.columns.push_back(test::MakeArray(columns.types[i], columns.values[i]).ValueOrDie());
  }
  batch.length = columns.values.empty() ? 0 : static_cast<int64_t>(columns.values[0].size());
  if (selection) {
    batch.selection = SelectionVector::Make(*selection, batch.length).ValueOrDie();
  }
  return batch;
}

std::vector<Value> Values(const BoundExpr& expr, const Batch& batch) {
  const auto array = Evaluate(expr, batch, arrow::default_memory_pool());
  EXPECT_TRUE(array.ok()) << array.status().ToString() << " for " << expr.ToString();
  if (!array.ok()) return {};
  EXPECT_TRUE((*array)->type()->Equals(*expr.type.ToArrow())) << expr.ToString();
  EXPECT_EQ((*array)->length(), batch.active_rows());
  EXPECT_TRUE((*array)->ValidateFull().ok());
  return test::ArrayToValues(**array).ValueOrDie();
}

void ExpectValues(const std::vector<Value>& actual, const std::vector<Value>& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (size_t i = 0; i < actual.size(); ++i) {
    EXPECT_TRUE(test::ValuesEqual(actual[i], expected[i]))
        << "row " << i << ": got " << test::ValueToString(actual[i]) << ", expected "
        << test::ValueToString(expected[i]);
  }
}

// --- three-valued logic --------------------------------------------------------------------------

// All nine pairs of {TRUE, FALSE, NULL}, in the order the expected tables list them.
Columns AllPairs() {
  const std::vector<Value> tri = {T(), F(), kNull};
  Columns c{{kBool, kBool}, {{}, {}}};
  for (const auto& a : tri) {
    for (const auto& b : tri) {
      c.values[0].push_back(a);
      c.values[1].push_back(b);
    }
  }
  return c;
}

TEST(EvalTest, AndTruthTable) {
  const auto result =
      Values(*Bin(BinaryOp::kAnd, Col(0, kBool), Col(1, kBool)), MakeBatch(AllPairs()));
  //                 T,T  T,F  T,N    F,T  F,F  F,N    N,T  N,F  N,N
  ExpectValues(result, {T(), F(), kNull, F(), F(), F(), kNull, F(), kNull});
}

TEST(EvalTest, OrTruthTable) {
  const auto result =
      Values(*Bin(BinaryOp::kOr, Col(0, kBool), Col(1, kBool)), MakeBatch(AllPairs()));
  ExpectValues(result, {T(), T(), T(), T(), F(), kNull, T(), kNull, kNull});
}

TEST(EvalTest, NotTruthTable) {
  Columns c{{kBool}, {{T(), F(), kNull}}};
  ExpectValues(Values(*Must(MakeNot(Col(0, kBool))), MakeBatch(c)), {F(), T(), kNull});
}

TEST(EvalTest, IsNullAndIsNotNull) {
  Columns c{{kInt}, {{I(1), kNull, I(3)}}};
  ExpectValues(Values(*Must(MakeIsNull(Col(0, kInt), false)), MakeBatch(c)), {F(), T(), F()});
  ExpectValues(Values(*Must(MakeIsNull(Col(0, kInt), true)), MakeBatch(c)), {T(), F(), T()});
}

// --- BETWEEN and IN --------------------------------------------------------------------------

TEST(EvalTest, BetweenIsInclusiveAtBothEnds) {
  Columns c{{kInt}, {{I(4), I(5), I(6), I(7), I(8), kNull}}};
  const auto expr = Must(MakeBetween(Col(0, kInt), IntLit(5), IntLit(7)));
  ExpectValues(Values(*expr, MakeBatch(c)), {F(), T(), T(), T(), F(), kNull});
}

TEST(EvalTest, BetweenWithNullBounds) {
  Columns c{{kInt}, {{I(5)}}};
  const auto null_int = Lit(arrow::MakeNullScalar(arrow::int32()));
  // 5 BETWEEN NULL AND 3 is FALSE (5 <= 3 is FALSE); 5 BETWEEN 1 AND NULL is NULL.
  ExpectValues(Values(*Must(MakeBetween(Col(0, kInt), null_int->Clone(), IntLit(3))), MakeBatch(c)),
               {F()});
  ExpectValues(Values(*Must(MakeBetween(Col(0, kInt), IntLit(1), null_int->Clone())), MakeBatch(c)),
               {kNull});
}

TEST(EvalTest, BetweenOnDatesAndDecimals) {
  const int32_t d0 = ParseDate("1994-01-01").ValueOrDie();
  Columns c{{kDate, Dec(5, 2)},
            {{Value(DateValue{d0 - 1}), Value(DateValue{d0}), Value(DateValue{d0 + 364}),
              Value(DateValue{d0 + 365})},
             {D(4, 2), D(5, 2), D(7, 2), D(8, 2)}}};
  const auto date_expr =
      Must(MakeBetween(Col(0, kDate), Lit(std::make_shared<arrow::Date32Scalar>(d0)),
                       Lit(std::make_shared<arrow::Date32Scalar>(d0 + 364))));
  ExpectValues(Values(*date_expr, MakeBatch(c)), {F(), T(), T(), F()});
  const auto decimal_expr = Must(MakeBetween(Col(1, Dec(5, 2)), DecLit("0.05"), DecLit("0.07")));
  ExpectValues(Values(*decimal_expr, MakeBatch(c)), {F(), T(), T(), F()});
}

std::shared_ptr<arrow::Scalar> I32(int32_t v) { return std::make_shared<arrow::Int32Scalar>(v); }

TEST(EvalTest, InListWithANullOperandIsNull) {
  Columns c{{kInt}, {{I(1), I(2), kNull, I(3)}}};
  const auto expr = Must(MakeInList(Col(0, kInt), {I32(1), I32(3)}));
  ExpectValues(Values(*expr, MakeBatch(c)), {T(), F(), kNull, T()});
}

TEST(EvalTest, InListWithANullValue) {
  Columns c{{kInt}, {{I(1), I(2)}}};
  auto with_null = Must(MakeInList(Col(0, kInt), {I32(1), arrow::MakeNullScalar(arrow::int32())}));
  // 1 IN (1, NULL) is TRUE, 2 IN (1, NULL) is NULL.
  ExpectValues(Values(*with_null, MakeBatch(c)), {T(), kNull});
  // NOT IN: 2 NOT IN (1, NULL) is NULL, 1 NOT IN (1, NULL) is FALSE.
  const auto negated = Must(MakeNot(std::move(with_null)));
  ExpectValues(Values(*negated, MakeBatch(c)), {F(), kNull});
}

TEST(EvalTest, InListOfStringsAndMixedNumericTypes) {
  Columns c{{kStr, Dec(5, 2)},
            {{S("a"), S("b"), S(""), kNull}, {D(150, 2), D(200, 2), D(0, 2), kNull}}};
  const auto strings = Must(MakeInList(Col(0, kStr), {std::make_shared<arrow::StringScalar>("b"),
                                                      std::make_shared<arrow::StringScalar>("")}));
  ExpectValues(Values(*strings, MakeBatch(c)), {F(), T(), T(), kNull});
  // DECIMAL(5,2) IN (INTEGER 2, DECIMAL 1.5): the list is cast to the common type.
  const auto numbers = Must(
      MakeInList(Col(1, Dec(5, 2)), {I32(2), std::make_shared<arrow::Decimal64Scalar>(
                                                 arrow::Decimal64(15), arrow::decimal64(2, 1))}));
  ExpectValues(Values(*numbers, MakeBatch(c)), {T(), T(), F(), kNull});
}

// --- comparisons and arithmetic ------------------------------------------------------------------

TEST(EvalTest, ComparesDecimalsOfDifferentScales) {
  // 1.50 = 1.5 is TRUE; 1.50 < 1.6; 2 > 1.99.
  Columns c{
      {Dec(5, 2), Dec(3, 1), kInt},
      {{D(150, 2), D(150, 2), D(199, 2)}, {D(15, 1), D(16, 1), D(15, 1)}, {I(2), I(2), I(2)}}};
  ExpectValues(Values(*Bin(BinaryOp::kEq, Col(0, Dec(5, 2)), Col(1, Dec(3, 1))), MakeBatch(c)),
               {T(), F(), F()});
  ExpectValues(Values(*Bin(BinaryOp::kLt, Col(0, Dec(5, 2)), Col(1, Dec(3, 1))), MakeBatch(c)),
               {F(), T(), F()});
  ExpectValues(Values(*Bin(BinaryOp::kGt, Col(2, kInt), Col(0, Dec(5, 2))), MakeBatch(c)),
               {T(), T(), T()});
  EXPECT_EQ(Bin(BinaryOp::kEq, DecLit("1.50"), DecLit("1.5"))->type, kBool);
  Columns one{{kInt}, {{I(0)}}};
  ExpectValues(Values(*Bin(BinaryOp::kEq, DecLit("1.50"), DecLit("1.5")), MakeBatch(one)), {T()});
}

TEST(EvalTest, AllSixComparisonsOnEveryType) {
  struct Case {
    LogicalType type;
    Value small;
    Value big;
  };
  const std::vector<Case> cases = {
      {kBool, F(), T()},
      {kInt, I(-3), I(4)},
      {kBig, L(-3), L(4)},
      {Dec(10, 2), D(-300, 2), D(400, 2)},
      {Dec(30, 2), D(-300, 2), D(400, 2)},
      {kDbl, Value(-0.5), Value(0.25)},
      {kDate, Value(DateValue{-3}), Value(DateValue{4})},
      {kStr, S("ab"), S("b")},
  };
  for (const auto& test : cases) {
    // rows: (small, big), (big, small), (small, small)
    Columns c{{test.type, test.type},
              {{test.small, test.big, test.small}, {test.big, test.small, test.small}}};
    const auto expr = [&](BinaryOp op) { return Bin(op, Col(0, test.type), Col(1, test.type)); };
    const auto name = test.type.ToString();
    ExpectValues(Values(*expr(BinaryOp::kEq), MakeBatch(c)), {F(), F(), T()});
    ExpectValues(Values(*expr(BinaryOp::kNe), MakeBatch(c)), {T(), T(), F()});
    ExpectValues(Values(*expr(BinaryOp::kLt), MakeBatch(c)), {T(), F(), F()});
    ExpectValues(Values(*expr(BinaryOp::kLe), MakeBatch(c)), {T(), F(), T()});
    ExpectValues(Values(*expr(BinaryOp::kGt), MakeBatch(c)), {F(), T(), F()});
    ExpectValues(Values(*expr(BinaryOp::kGe), MakeBatch(c)), {F(), T(), T()});
  }
}

TEST(EvalTest, ComparisonWithNullIsNull) {
  Columns c{{kInt, kInt}, {{I(1), kNull, kNull}, {kNull, I(1), kNull}}};
  ExpectValues(Values(*Bin(BinaryOp::kEq, Col(0, kInt), Col(1, kInt)), MakeBatch(c)),
               {kNull, kNull, kNull});
}

TEST(EvalTest, TpchStyleDecimalExpression) {
  // l_extendedprice * (1 - l_discount) * (1 + l_tax), the shape of TPC-H Q1.
  const LogicalType price = Dec(15, 2);
  Columns c{{price, price, price},
            {{D(10000, 2), D(250050, 2), kNull},
             {D(5, 2), D(10, 2), D(5, 2)},
             {D(8, 2), D(0, 2), D(8, 2)}}};
  auto discounted =
      Bin(BinaryOp::kMul, Col(0, price), Bin(BinaryOp::kSub, IntLit(1), Col(1, price)));
  EXPECT_EQ(discounted->type, Dec(18, 4));
  auto charge =
      Bin(BinaryOp::kMul, std::move(discounted), Bin(BinaryOp::kAdd, IntLit(1), Col(2, price)));
  EXPECT_EQ(charge->type, Dec(18, 6));
  // 100.00 * 0.95 * 1.08 = 102.6; 2500.50 * 0.90 * 1.00 = 2250.45
  ExpectValues(Values(*charge, MakeBatch(c)), {D(102600000, 6), D(2250450000, 6), kNull});
}

TEST(EvalTest, WideDecimalMultiplicationUses128Bits) {
  const LogicalType wide = Dec(31, 4);
  const LogicalType narrow = Dec(16, 2);
  Columns c{{wide, narrow}, {{D(PowerOfTen<Int128>(20), 4), D(-3, 4)}, {D(250, 2), D(1000000, 2)}}};
  auto product = Bin(BinaryOp::kMul, Col(0, wide), Col(1, narrow));
  EXPECT_EQ(product->type, Dec(38, 6));
  // 10^16 * 2.50 = 2.5 * 10^16 at scale 6; -0.0003 * 10000.00 = -3
  ExpectValues(Values(*product, MakeBatch(c)),
               {D(PowerOfTen<Int128>(20) * 250, 6), D(-3 * 1000000, 6)});
}

TEST(EvalTest, DecimalOverflowIsAnError) {
  const LogicalType wide = Dec(38, 0);
  Columns c{{wide, wide}, {{D(PowerOfTen<Int128>(20), 0)}, {D(PowerOfTen<Int128>(20), 0)}}};
  const auto result = Evaluate(*Bin(BinaryOp::kMul, Col(0, wide), Col(1, wide)), MakeBatch(c),
                               arrow::default_memory_pool());
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(result.status().IsInvalid());
  EXPECT_THAT(result.status().message(), HasSubstr("decimal overflow"));

  // 64-bit: 10^9 * 10^9 = 10^18 needs 19 digits in DECIMAL(18,0)
  const LogicalType narrow = Dec(18, 0);
  Columns d{{narrow, narrow}, {{D(1000000000, 0)}, {D(1000000000, 0)}}};
  const auto narrow_result = Evaluate(*Bin(BinaryOp::kMul, Col(0, narrow), Col(1, narrow)),
                                      MakeBatch(d), arrow::default_memory_pool());
  ASSERT_FALSE(narrow_result.ok());
  EXPECT_THAT(narrow_result.status().message(), HasSubstr("decimal overflow"));
}

TEST(EvalTest, IntegerOverflowIsAnErrorButNullRowsAreIgnored) {
  Columns c{{kInt, kInt}, {{I(INT32_MAX)}, {I(1)}}};
  const auto overflow = Evaluate(*Bin(BinaryOp::kAdd, Col(0, kInt), Col(1, kInt)), MakeBatch(c),
                                 arrow::default_memory_pool());
  ASSERT_FALSE(overflow.ok());
  EXPECT_THAT(overflow.status().message(), HasSubstr("overflow"));
  // A NULL operand hides the row: INT32_MAX + NULL is NULL, not an error.
  Columns n{{kInt, kInt}, {{I(INT32_MAX), I(5)}, {kNull, I(6)}}};
  ExpectValues(Values(*Bin(BinaryOp::kAdd, Col(0, kInt), Col(1, kInt)), MakeBatch(n)),
               {kNull, I(11)});
  // INTEGER meeting BIGINT is computed in BIGINT and does not overflow.
  Columns mixed{{kInt, kBig}, {{I(INT32_MAX)}, {L(1)}}};
  ExpectValues(Values(*Bin(BinaryOp::kAdd, Col(0, kInt), Col(1, kBig)), MakeBatch(mixed)),
               {L(int64_t{INT32_MAX} + 1)});
}

TEST(EvalTest, DoubleArithmeticAndMixedTypes) {
  Columns c{{kDbl, kInt, Dec(5, 2)},
            {{Value(0.5), Value(-1.25)}, {I(3), I(2)}, {D(150, 2), D(25, 2)}}};
  ExpectValues(Values(*Bin(BinaryOp::kMul, Col(0, kDbl), Col(1, kInt)), MakeBatch(c)),
               {Value(1.5), Value(-2.5)});
  ExpectValues(Values(*Bin(BinaryOp::kAdd, Col(0, kDbl), Col(2, Dec(5, 2))), MakeBatch(c)),
               {Value(2.0), Value(-1.0)});
  ExpectValues(Values(*Bin(BinaryOp::kSub, Col(1, kInt), Col(0, kDbl)), MakeBatch(c)),
               {Value(2.5), Value(3.25)});
}

TEST(EvalTest, DecimalToDoubleCastRoundsOnce) {
  // The unscaled values are above 2^53: dividing their double by 10^scale would round twice.
  Columns c{{Dec(17, 10), Dec(38, 6)},
            {{D(10040364599609375, 10), kNull, D(-10040364599609375, 10)},
             {D(PowerOfTen<Int128>(37) + 1000, 6), D(5, 6), kNull}}};
  ExpectValues(Values(*Must(MakeCast(Col(0, Dec(17, 10)), kDbl)), MakeBatch(c)),
               {Value(1004036.4599609375), kNull, Value(-1004036.4599609375)});
  ExpectValues(Values(*Must(MakeCast(Col(1, Dec(38, 6)), kDbl)), MakeBatch(c)),
               {Value(1e31), Value(5e-6), kNull});
}

TEST(EvalTest, NegateAndCast) {
  Columns c{{kInt, Dec(5, 2), kDbl},
            {{I(5), kNull}, {D(-150, 2), D(25, 2)}, {Value(0.0), Value(2.5)}}};
  ExpectValues(Values(*Must(MakeNegate(Col(0, kInt))), MakeBatch(c)), {I(-5), kNull});
  ExpectValues(Values(*Must(MakeNegate(Col(1, Dec(5, 2)))), MakeBatch(c)), {D(150, 2), D(-25, 2)});
  ExpectValues(Values(*Must(MakeNegate(Col(2, kDbl))), MakeBatch(c)), {Value(-0.0), Value(-2.5)});
  ExpectValues(Values(*Must(MakeCast(Col(1, Dec(5, 2)), kDbl)), MakeBatch(c)),
               {Value(-1.5), Value(0.25)});
  ExpectValues(Values(*Must(MakeCast(Col(0, kInt), Dec(10, 3))), MakeBatch(c)),
               {D(5000, 3), kNull});
  ExpectValues(Values(*Must(MakeCast(Col(0, kInt), kBig)), MakeBatch(c)), {L(5), kNull});
  // Rescaling rounds half away from zero: -1.50 -> -2 (scale 0), 0.25 -> 0.3 (scale 1).
  ExpectValues(Values(*Must(MakeCast(Col(1, Dec(5, 2)), Dec(5, 0))), MakeBatch(c)),
               {D(-2, 0), D(0, 0)});
  ExpectValues(Values(*Must(MakeCast(Col(1, Dec(5, 2)), Dec(5, 1))), MakeBatch(c)),
               {D(-15, 1), D(3, 1)});
  EXPECT_FALSE(MakeCast(Col(2, kDbl), kInt).ok());
  EXPECT_FALSE(MakeCast(Col(0, kInt), kStr).ok());
  // A cast that does not fit is an error: 5 as DECIMAL(2,2) would need three digits.
  const auto too_narrow = Evaluate(*Must(MakeCast(Col(0, kInt), Dec(2, 2))), MakeBatch(c),
                                   arrow::default_memory_pool());
  ASSERT_FALSE(too_narrow.ok());
  EXPECT_THAT(too_narrow.status().message(), HasSubstr("decimal overflow"));
}

TEST(EvalTest, DateArithmetic) {
  const int32_t d = ParseDate("1998-12-01").ValueOrDie();
  Columns c{{kDate}, {{Value(DateValue{d}), kNull}}};
  const auto minus = Bin(BinaryOp::kSub, Col(0, kDate), IntLit(90));
  EXPECT_EQ(minus->type, kDate);
  ExpectValues(Values(*minus, MakeBatch(c)),
               {Value(DateValue{ParseDate("1998-09-02").ValueOrDie()}), kNull});
  ExpectValues(Values(*Bin(BinaryOp::kAdd, IntLit(1), Col(0, kDate)), MakeBatch(c)),
               {Value(DateValue{d + 1}), kNull});
}

// --- batches, selection vectors
// ---------------------------------------------------------------------

TEST(EvalTest, SelectionVectorReturnsOnlySelectedRowsInOrder) {
  Columns c{
      {kInt, kStr, Dec(5, 2)},
      {{I(0), I(1), I(2), I(3), I(4), I(5), I(6), I(7), I(8), I(9)},
       {S("a"), S("b"), S("c"), S("d"), S("e"), S("f"), S("g"), S("h"), S("i"), S("j")},
       {D(0, 2), D(1, 2), D(2, 2), D(3, 2), D(4, 2), D(5, 2), D(6, 2), D(7, 2), D(8, 2), D(9, 2)}}};
  const Batch batch = MakeBatch(c, std::vector<uint32_t>{1, 4, 5, 9});
  ExpectValues(Values(*Bin(BinaryOp::kMul, Col(0, kInt), IntLit(2)), batch),
               {I(2), I(8), I(10), I(18)});
  ExpectValues(Values(*Col(1, kStr), batch), {S("b"), S("e"), S("f"), S("j")});
  ExpectValues(Values(*Col(2, Dec(5, 2)), batch), {D(1, 2), D(4, 2), D(5, 2), D(9, 2)});
  // A literal is broadcast to the live rows only.
  ExpectValues(Values(*IntLit(7), batch), {I(7), I(7), I(7), I(7)});
}

TEST(EvalTest, SelectionOfTheLastRowAndOfASingleRow) {
  Columns c{{kInt}, {{I(10), I(20), I(30)}}};
  ExpectValues(Values(*Col(0, kInt), MakeBatch(c, std::vector<uint32_t>{2})), {I(30)});
  ExpectValues(Values(*Col(0, kInt), MakeBatch(c, std::vector<uint32_t>{0, 1})), {I(10), I(20)});
  ExpectValues(Values(*Bin(BinaryOp::kAdd, Col(0, kInt), IntLit(1)),
                      MakeBatch(c, std::vector<uint32_t>{0, 2})),
               {I(11), I(31)});
}

TEST(EvalTest, EmptyBatchAndAllFilteredSelectionReturnEmptyArrays) {
  Columns empty{{kInt, Dec(5, 2), kStr, kBool}, {{}, {}, {}, {}}};
  Columns full{{kInt, Dec(5, 2), kStr, kBool},
               {{I(1), I(2)}, {D(1, 2), D(2, 2)}, {S("a"), S("b")}, {T(), F()}}};
  for (const Batch& batch : {MakeBatch(empty), MakeBatch(full, std::vector<uint32_t>{})}) {
    EXPECT_EQ(batch.active_rows(), 0);
    ExpectValues(Values(*Col(0, kInt), batch), {});
    ExpectValues(Values(*Bin(BinaryOp::kMul, Col(1, Dec(5, 2)), Col(1, Dec(5, 2))), batch), {});
    ExpectValues(Values(*Bin(BinaryOp::kLt, Col(2, kStr), Col(2, kStr)), batch), {});
    ExpectValues(Values(*Must(MakeNot(Col(3, kBool))), batch), {});
    ExpectValues(Values(*Bin(BinaryOp::kAnd, Col(3, kBool), Col(3, kBool)), batch), {});
    ExpectValues(Values(*Must(MakeBetween(Col(0, kInt), IntLit(1), IntLit(2))), batch), {});
    ExpectValues(Values(*Must(MakeIsNull(Col(0, kInt), false)), batch), {});
    ExpectValues(Values(*Must(MakeCast(Col(0, kInt), kDbl)), batch), {});
    ExpectValues(Values(*Must(MakeInList(Col(0, kInt), {I32(1)})), batch), {});
    ExpectValues(Values(*IntLit(1), batch), {});
    const auto selection = EvaluatePredicate(*Bin(BinaryOp::kLt, Col(0, kInt), IntLit(5)), batch,
                                             arrow::default_memory_pool())
                               .ValueOrDie();
    EXPECT_EQ(selection.size(), 0);
  }
}

TEST(EvalTest, EvaluatePredicateKeepsTrueRowsAsBatchIndices) {
  Columns c{{kInt}, {{I(1), I(9), kNull, I(3), I(7), I(8)}}};
  const auto predicate = Bin(BinaryOp::kGt, Col(0, kInt), IntLit(2));
  const auto all =
      EvaluatePredicate(*predicate, MakeBatch(c), arrow::default_memory_pool()).ValueOrDie();
  EXPECT_EQ(std::vector<uint32_t>(all.indices().begin(), all.indices().end()),
            (std::vector<uint32_t>{1, 3, 4, 5}));
  // With a selection the result still indexes the batch, not the selection.
  const auto some = EvaluatePredicate(*predicate, MakeBatch(c, std::vector<uint32_t>{0, 1, 2, 4}),
                                      arrow::default_memory_pool())
                        .ValueOrDie();
  EXPECT_EQ(std::vector<uint32_t>(some.indices().begin(), some.indices().end()),
            (std::vector<uint32_t>{1, 4}));
  EXPECT_TRUE(EvaluatePredicate(*Col(0, kInt), MakeBatch(c), arrow::default_memory_pool())
                  .status()
                  .IsInvalid());
}

TEST(EvalTest, NullLiteralIsTypedAndPropagates) {
  Columns c{{kInt}, {{I(1), I(2)}}};
  const auto null_int = Lit(arrow::MakeNullScalar(arrow::int32()));
  ExpectValues(Values(*Bin(BinaryOp::kAdd, Col(0, kInt), null_int->Clone()), MakeBatch(c)),
               {kNull, kNull});
  const auto null_bool = Lit(arrow::MakeNullScalar(arrow::boolean()));
  ExpectValues(Values(*null_bool, MakeBatch(c)), {kNull, kNull});
}

TEST(EvalTest, ReadsAParquetStyleDecimal128ColumnOfSmallPrecision) {
  // The Parquet reader returns decimal128(15, 2) for DECIMAL(15,2).
  arrow::Decimal128Builder builder(arrow::decimal128(15, 2));
  ASSERT_TRUE(builder.Append(arrow::Decimal128(12345)).ok());
  ASSERT_TRUE(builder.AppendNull().ok());
  ASSERT_TRUE(builder.Append(arrow::Decimal128(-5)).ok());
  Batch batch;
  batch.columns.push_back(builder.Finish().ValueOrDie());
  batch.length = 3;
  const LogicalType type = Dec(15, 2);
  ExpectValues(Values(*Col(0, type), batch), {D(12345, 2), kNull, D(-5, 2)});
  ExpectValues(Values(*Bin(BinaryOp::kMul, Col(0, type), DecLit("2.0")), batch),
               {D(246900, 3), kNull, D(-100, 3)});
  batch.selection = SelectionVector::Make({0, 2}, 3).ValueOrDie();
  ExpectValues(Values(*Col(0, type), batch), {D(12345, 2), D(-5, 2)});
}

TEST(EvalTest, MismatchedColumnsAreInvalidNotUndefined) {
  Columns c{{kInt}, {{I(1)}}};
  // The expression says BIGINT, the batch holds INTEGER.
  EXPECT_TRUE(
      Evaluate(*Col(0, kBig), MakeBatch(c), arrow::default_memory_pool()).status().IsInvalid());
  EXPECT_TRUE(
      Evaluate(*Col(3, kInt), MakeBatch(c), arrow::default_memory_pool()).status().IsInvalid());
  EXPECT_TRUE(
      Evaluate(*Col(-1, kInt), MakeBatch(c), arrow::default_memory_pool()).status().IsInvalid());
}

TEST(EvalTest, ToStringRendersTheTree) {
  const auto expr = Bin(BinaryOp::kGe, Col(0, Dec(5, 2)), DecLit("0.05"));
  EXPECT_EQ(expr->ToString(), "(#0 >= CAST(0.05 AS DECIMAL(5,2)))");
  const auto sum = Bin(BinaryOp::kAdd, Col(0, kInt), Col(1, kBig));
  EXPECT_EQ(sum->ToString(), "(CAST(#0 AS BIGINT) + #1)");
  EXPECT_EQ(Must(MakeBetween(Col(0, kInt), IntLit(1), IntLit(2)))->ToString(),
            "(#0 BETWEEN 1 AND 2)");
  EXPECT_EQ(sum->Clone()->ToString(), sum->ToString());
}

TEST(EvalTest, FactoriesRejectIllTypedTrees) {
  EXPECT_FALSE(MakeBinary(BinaryOp::kAdd, Col(0, kStr), Col(1, kInt)).ok());
  EXPECT_FALSE(MakeBinary(BinaryOp::kAnd, Col(0, kInt), Col(1, kInt)).ok());
  EXPECT_FALSE(MakeBinary(BinaryOp::kEq, Col(0, kStr), Col(1, kInt)).ok());
  EXPECT_FALSE(MakeNot(Col(0, kInt)).ok());
  EXPECT_FALSE(MakeNegate(Col(0, kStr)).ok());
  EXPECT_FALSE(MakeInList(Col(0, kInt), {}).ok());
  EXPECT_FALSE(MakeLiteral(std::make_shared<arrow::FloatScalar>(1.0f)).ok());
}

}  // namespace
}  // namespace quarry
