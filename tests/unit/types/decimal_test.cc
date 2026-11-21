#include "types/decimal.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

namespace quarry {
namespace {

using ::testing::HasSubstr;

template <typename T>
class DecimalArithmeticTest : public ::testing::Test {};

using Representations = ::testing::Types<int64_t, Int128>;
TYPED_TEST_SUITE(DecimalArithmeticTest, Representations);

TYPED_TEST(DecimalArithmeticTest, AddSubMulOnScaledIntegers) {
  using T = TypeParam;
  // 1.25 + 2.50 and 1.25 - 2.50 at scale 2.
  EXPECT_EQ(DecimalAdd<T>(125, 250, 18).ValueOrDie(), 375);
  EXPECT_EQ(DecimalSub<T>(125, 250, 18).ValueOrDie(), -125);
  // 1.25 * 2.50 = 3.1250 at scale 4.
  EXPECT_EQ(DecimalMul<T>(125, 250, 18).ValueOrDie(), 31250);
  EXPECT_EQ(DecimalMul<T>(-125, 250, 18).ValueOrDie(), -31250);
}

TYPED_TEST(DecimalArithmeticTest, ResultMustFitThePrecision) {
  using T = TypeParam;
  // 99 + 1 needs three digits.
  EXPECT_EQ(DecimalAdd<T>(99, 1, 3).ValueOrDie(), 100);
  const auto add = DecimalAdd<T>(99, 1, 2);
  ASSERT_FALSE(add.ok());
  EXPECT_TRUE(add.status().IsInvalid());
  EXPECT_THAT(add.status().message(), HasSubstr("decimal overflow"));
  EXPECT_FALSE(DecimalSub<T>(-99, 1, 2).ok());
  EXPECT_FALSE(DecimalMul<T>(10, 10, 2).ok());
  EXPECT_TRUE(DecimalMul<T>(10, 9, 2).ok());
}

TEST(DecimalTest, Int64ArithmeticDoesNotWrapAround) {
  const int64_t big = 4'000'000'000'000'000'000;
  EXPECT_THAT(DecimalAdd<int64_t>(big, big, 18).status().message(), HasSubstr("decimal overflow"));
  EXPECT_THAT(DecimalMul<int64_t>(big, 3, 18).status().message(), HasSubstr("decimal overflow"));
}

TEST(DecimalTest, Decimal38Multiplication) {
  const Int128 ten_to_19 = PowerOfTen<Int128>(19);
  // 10^19 * 10^19 = 10^38 needs 39 digits.
  const auto product = DecimalMul<Int128>(ten_to_19, ten_to_19, 38);
  ASSERT_FALSE(product.ok());
  EXPECT_TRUE(product.status().IsInvalid());
  EXPECT_THAT(product.status().message(), HasSubstr("decimal overflow"));
  // 10^19 * 10^18 = 10^37 fits.
  EXPECT_EQ(DecimalMul<Int128>(ten_to_19, PowerOfTen<Int128>(18), 38).ValueOrDie(),
            PowerOfTen<Int128>(37));
  // A product beyond 128 bits is detected, not wrapped.
  const Int128 huge = PowerOfTen<Int128>(30);
  EXPECT_THAT(DecimalMul<Int128>(huge, huge, 38).status().message(), HasSubstr("decimal overflow"));
}

TEST(DecimalTest, PowersOfTen) {
  EXPECT_EQ(PowerOfTen<int64_t>(18), 1'000'000'000'000'000'000);
  EXPECT_EQ(DecimalToString(PowerOfTen<Int128>(38), 0), "1" + std::string(38, '0'));
  EXPECT_TRUE(FitsPrecision<Int128>(PowerOfTen<Int128>(38) - 1, 38));
  EXPECT_FALSE(FitsPrecision<Int128>(PowerOfTen<Int128>(38), 38));
  EXPECT_FALSE(FitsPrecision<int64_t>(-PowerOfTen<int64_t>(18), 18));
}

// The rounding DuckDB 2.0.1 shows (docs/sql.md): half away from zero.
TEST(DecimalTest, RescaleRoundsHalfAwayFromZero) {
  // CAST(1.005 AS DECIMAL(4,2)) = 1.01, CAST(-1.005 AS DECIMAL(4,2)) = -1.01
  EXPECT_EQ(DecimalRescale<int64_t>(1005, 3, 2, 4).ValueOrDie(), 101);
  EXPECT_EQ(DecimalRescale<int64_t>(-1005, 3, 2, 4).ValueOrDie(), -101);
  EXPECT_EQ(DecimalRescale<int64_t>(1015, 3, 2, 4).ValueOrDie(), 102);
  EXPECT_EQ(DecimalRescale<int64_t>(1004, 3, 2, 4).ValueOrDie(), 100);
  EXPECT_EQ(DecimalRescale<int64_t>(-1004, 3, 2, 4).ValueOrDie(), -100);
  // CAST(2.5 AS DECIMAL(2,0)) = 3, CAST(-2.5 AS DECIMAL(2,0)) = -3
  EXPECT_EQ(DecimalRescale<int64_t>(25, 1, 0, 2).ValueOrDie(), 3);
  EXPECT_EQ(DecimalRescale<int64_t>(-25, 1, 0, 2).ValueOrDie(), -3);
  EXPECT_EQ(DecimalRescale<int64_t>(5, 1, 0, 1).ValueOrDie(), 1);
  EXPECT_EQ(DecimalRescale<int64_t>(4, 1, 0, 1).ValueOrDie(), 0);
  // The same on the 128-bit representation, including dropping 38 digits at once.
  EXPECT_EQ(DecimalRescale<Int128>(1005, 3, 2, 38).ValueOrDie(), 101);
  EXPECT_EQ(DecimalRescale<Int128>(-25, 1, 0, 38).ValueOrDie(), -3);
  EXPECT_EQ(DecimalRescale<Int128>(PowerOfTen<Int128>(38) - 1, 38, 0, 1).ValueOrDie(), 1);
}

TEST(DecimalTest, RescaleUpAndPrecisionOverflow) {
  EXPECT_EQ(DecimalRescale<int64_t>(15, 1, 4, 18).ValueOrDie(), 15000);
  EXPECT_EQ(DecimalRescale<int64_t>(7, 0, 2, 3).ValueOrDie(), 700);
  // CAST(99.99 AS DECIMAL(3,1)): 99.99 rounds to 100.0, which needs four digits (DuckDB: out of
  // range).
  const auto overflow = DecimalRescale<int64_t>(9999, 2, 1, 3);
  ASSERT_FALSE(overflow.ok());
  EXPECT_THAT(overflow.status().message(), HasSubstr("decimal overflow"));
  EXPECT_THAT(DecimalRescale<int64_t>(7, 0, 2, 2).status().message(),
              HasSubstr("decimal overflow"));
  // Scaling up beyond 64 bits.
  int64_t out = 0;
  EXPECT_FALSE(RescaleValue<int64_t>(PowerOfTen<int64_t>(18), 0, 5, &out));
  Int128 wide = 0;
  EXPECT_TRUE(RescaleValue<Int128>(PowerOfTen<int64_t>(18), 0, 5, &wide));
  EXPECT_EQ(wide, PowerOfTen<Int128>(23));
}

TEST(DecimalTest, ToStringAndParse) {
  EXPECT_EQ(DecimalToString(-12345, 2), "-123.45");
  EXPECT_EQ(DecimalToString(7, 0), "7");
  EXPECT_EQ(DecimalToString(5, 3), "0.005");
  EXPECT_EQ(DecimalToString(0, 2), "0.00");
  int scale = -1;
  EXPECT_EQ(ParseDecimalDigits("0.05", &scale).ValueOrDie(), 5);
  EXPECT_EQ(scale, 2);
  EXPECT_EQ(ParseDecimalDigits("-12", &scale).ValueOrDie(), -12);
  EXPECT_EQ(scale, 0);
  EXPECT_EQ(ParseDecimalDigits("1.", &scale).ValueOrDie(), 1);
  EXPECT_EQ(scale, 0);
  EXPECT_TRUE(ParseDecimalDigits("1.2.3", &scale).status().IsInvalid());
  EXPECT_TRUE(ParseDecimalDigits("abc", &scale).status().IsInvalid());
  EXPECT_TRUE(ParseDecimalDigits("", &scale).status().IsInvalid());
  EXPECT_TRUE(ParseDecimalDigits(std::string(39, '9'), &scale).status().IsInvalid());
  EXPECT_TRUE(ParseDecimalDigits(std::string(38, '9'), &scale).ok());
}

TEST(DecimalTest, ToDoubleIsCorrectlyRounded) {
  // Small values: one exact division.
  EXPECT_EQ(DecimalToDouble(12345, 2), 123.45);
  EXPECT_EQ(DecimalToDouble(-5, 3), -0.005);
  EXPECT_EQ(DecimalToDouble(0, 6), 0.0);
  // 1004036.4599609375 is k/1024, so a double holds it exactly, but its unscaled value
  // 10040364599609375 is above 2^53: converting that to a double first and dividing rounds twice
  // and lands one ulp away. DuckDB rounds once, and so must we.
  EXPECT_EQ(DecimalToDouble(10040364599609375, 10), 1004036.4599609375);
  EXPECT_EQ(DecimalToDouble(-10040364599609375, 10), -1004036.4599609375);
  EXPECT_EQ(DecimalToDouble(123456789123456789, 9), 123456789.123456789);
  // Beyond 64 bits, and with a scale whose power of ten is not a double.
  EXPECT_EQ(DecimalToDouble(PowerOfTen<Int128>(37), 0), 1e37);
  EXPECT_EQ(DecimalToDouble(PowerOfTen<Int128>(37) + 1000, 6), 1e31);
  EXPECT_EQ(DecimalToDouble(15, 30), 1.5e-29);
  EXPECT_EQ(DecimalToDouble(kInt128Max, 37), 17.014118346046924);
}

}  // namespace
}  // namespace quarry
