// Property test: the vectorized evaluator agrees with the row-at-a-time reference evaluator on
// random batches and random type-correct expression trees. The reference shares no code with the
// kernels (tests/support/reference_eval.cc), so a bug in either shows up as a disagreement.

#include <arrow/memory_pool.h>
#include <arrow/scalar.h>
#include <arrow/util/decimal.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <functional>
#include <random>
#include <string>
#include <vector>

#include "expr/bound_expr.h"
#include "expr/eval.h"
#include "support/reference_eval.h"
#include "support/value.h"
#include "types/decimal.h"

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

// The columns of every generated batch. The numeric ones are 1..7.
const std::vector<LogicalType>& ColumnTypes() {
  static const std::vector<LogicalType> types = {
      kBool,
      kInt,
      kInt,
      kBig,
      LogicalType::Decimal(9, 2),
      LogicalType::Decimal(15, 2),
      LogicalType::Decimal(25, 4),
      kDbl,
      kDate,
      kStr,
  };
  return types;
}
constexpr int kFirstNumeric = 1;
constexpr int kLastNumeric = 7;
constexpr int kDateColumn = 8;
constexpr int kStringColumn = 9;

class Random {
 public:
  explicit Random(uint64_t seed) : engine_(seed) {}
  int64_t Between(int64_t lo, int64_t hi) {
    return std::uniform_int_distribution<int64_t>(lo, hi)(engine_);
  }
  bool Chance(int percent) { return Between(0, 99) < percent; }
  template <typename T>
  const T& Pick(const std::vector<T>& items) {
    return items[static_cast<size_t>(Between(0, static_cast<int64_t>(items.size()) - 1))];
  }

 private:
  std::mt19937_64 engine_;
};

const std::vector<std::string>& Words() {
  static const std::vector<std::string> words = {"", "a", "b", "ab", "abc", "ba", "z"};
  return words;
}

Value RandomValue(Random& rng, const LogicalType& type) {
  switch (type.id) {
    case TypeId::kBoolean:
      return Value(rng.Chance(50));
    case TypeId::kInt32:
      return Value(static_cast<int32_t>(rng.Between(-1000, 1000)));
    case TypeId::kInt64:
      return Value(rng.Between(-1'000'000, 1'000'000));
    case TypeId::kDouble:
      return Value(static_cast<double>(rng.Between(-(1 << 20), 1 << 20)) / 1024.0);
    case TypeId::kDate:
      return Value(DateValue{static_cast<int32_t>(rng.Between(8000, 12000))});
    case TypeId::kVarchar:
      return Value(rng.Pick(Words()));
    case TypeId::kDecimal: {
      const int64_t limit = type.precision <= 9    ? 100'000
                            : type.precision <= 15 ? 10'000'000
                                                   : 10'000'000'000;
      return Value(DecimalValue{rng.Between(-limit, limit), type.scale});
    }
  }
  return Value{};
}

struct RandomBatch {
  std::vector<std::vector<Value>> columns;  // columns[c][row]
  Batch batch;
  std::vector<std::vector<Value>> active_rows;  // rows[r][c], in selection order
};

RandomBatch MakeRandomBatch(Random& rng, uint64_t seed) {
  RandomBatch out;
  const int64_t n = seed % 17 == 3 ? rng.Between(0, 4) : rng.Between(0, 3000);
  const int null_percent = static_cast<int>(rng.Between(0, 30));
  for (const auto& type : ColumnTypes()) {
    std::vector<Value> values;
    values.reserve(static_cast<size_t>(n));
    for (int64_t i = 0; i < n; ++i) {
      values.push_back(rng.Chance(null_percent) ? Value{} : RandomValue(rng, type));
    }
    // A quarter of the columns are slices with a non-zero offset.
    const int64_t garbage = rng.Chance(25) ? rng.Between(1, 9) : 0;
    out.batch.columns.push_back(test::MakeArray(type, values, garbage).ValueOrDie());
    out.columns.push_back(std::move(values));
  }
  out.batch.length = n;
  std::vector<uint32_t> live;
  if (rng.Chance(50)) {
    const int keep = static_cast<int>(rng.Between(0, 100));
    for (int64_t i = 0; i < n; ++i) {
      if (rng.Chance(keep)) live.push_back(static_cast<uint32_t>(i));
    }
    out.batch.selection = SelectionVector::Make(live, n).ValueOrDie();
  } else {
    for (int64_t i = 0; i < n; ++i) live.push_back(static_cast<uint32_t>(i));
  }
  for (const uint32_t row : live) {
    std::vector<Value> values;
    for (const auto& column : out.columns) values.push_back(column[row]);
    out.active_rows.push_back(std::move(values));
  }
  return out;
}

enum class Category { kLogical, kNumeric, kCalendar, kText };

class ExpressionGenerator {
 public:
  explicit ExpressionGenerator(Random& rng) : rng_(rng) {}

  ExprPtr Generate(Category category, int depth) {
    switch (category) {
      case Category::kLogical:
        return Bool(depth);
      case Category::kNumeric:
        return Number(depth);
      case Category::kCalendar:
        return Date(depth);
      case Category::kText:
        return String(depth);
    }
    return nullptr;
  }

 private:
  static ExprPtr Take(arrow::Result<ExprPtr> result, const std::function<ExprPtr()>& fallback) {
    return result.ok() ? std::move(result).ValueOrDie() : fallback();
  }

  Category AnyCategory() { return static_cast<Category>(rng_.Between(0, 3)); }

  ExprPtr Literal(std::shared_ptr<arrow::Scalar> scalar) {
    return MakeLiteral(std::move(scalar)).ValueOrDie();
  }

  std::shared_ptr<arrow::Scalar> RandomScalar(const LogicalType& type) {
    if (rng_.Chance(8)) return arrow::MakeNullScalar(type.ToArrow());
    const Value v = RandomValue(rng_, type);
    switch (type.id) {
      case TypeId::kBoolean:
        return std::make_shared<arrow::BooleanScalar>(std::get<bool>(v));
      case TypeId::kInt32:
        return std::make_shared<arrow::Int32Scalar>(static_cast<int32_t>(rng_.Between(-50, 50)));
      case TypeId::kInt64:
        return std::make_shared<arrow::Int64Scalar>(rng_.Between(-5000, 5000));
      case TypeId::kDouble:
        return std::make_shared<arrow::DoubleScalar>(std::get<double>(v));
      case TypeId::kDate:
        return std::make_shared<arrow::Date32Scalar>(std::get<DateValue>(v).days);
      case TypeId::kVarchar:
        return std::make_shared<arrow::StringScalar>(std::get<std::string>(v));
      case TypeId::kDecimal:
        return nullptr;  // built from digits by DecimalLiteral
    }
    return nullptr;
  }

  ExprPtr DecimalLiteral() {
    static const std::vector<std::string> digits = {"0.05", "1.50",  "12.345", "0.5",   "100",
                                                    "7.25", "0.000", "3.1",    "99.99", "0.01"};
    return MakeDecimalLiteral(rng_.Pick(digits)).ValueOrDie();
  }

  ExprPtr NumberLeaf() {
    const int64_t pick = rng_.Between(0, 9);
    if (pick <= 5) {
      const int column = static_cast<int>(rng_.Between(kFirstNumeric, kLastNumeric));
      return MakeColumn(column, ColumnTypes()[static_cast<size_t>(column)]);
    }
    switch (pick) {
      case 6:
        return Literal(RandomScalar(kInt));
      case 7:
        return Literal(RandomScalar(kBig));
      case 8:
        return Literal(RandomScalar(kDbl));
      default:
        return DecimalLiteral();
    }
  }

  ExprPtr Number(int depth) {
    if (depth <= 0 || rng_.Chance(25)) return NumberLeaf();
    const int64_t pick = rng_.Between(0, 9);
    if (pick <= 5) {
      static const std::vector<BinaryOp> ops = {BinaryOp::kAdd, BinaryOp::kSub, BinaryOp::kMul};
      return Take(MakeBinary(rng_.Pick(ops), Number(depth - 1), Number(depth - 1)),
                  [&] { return NumberLeaf(); });
    }
    if (pick <= 7) {
      return Take(MakeNegate(Number(depth - 1)), [&] { return NumberLeaf(); });
    }
    static const std::vector<LogicalType> targets = {kDbl,
                                                     kBig,
                                                     LogicalType::Decimal(18, 4),
                                                     LogicalType::Decimal(30, 6),
                                                     LogicalType::Decimal(12, 2),
                                                     LogicalType::Decimal(20, 5),
                                                     LogicalType::Decimal(8, 3)};
    return Take(MakeCast(Number(depth - 1), rng_.Pick(targets)), [&] { return NumberLeaf(); });
  }

  ExprPtr Date(int depth) {
    const int64_t pick = rng_.Between(0, 4);
    if (depth <= 0 || pick <= 1) return MakeColumn(kDateColumn, kDate);
    if (pick == 2) return Literal(RandomScalar(kDate));
    ExprPtr days = rng_.Chance(50) ? MakeColumn(1, kInt) : Literal(RandomScalar(kInt));
    const BinaryOp op = rng_.Chance(50) ? BinaryOp::kAdd : BinaryOp::kSub;
    return Take(MakeBinary(op, Date(depth - 1), std::move(days)),
                [&] { return MakeColumn(kDateColumn, kDate); });
  }

  ExprPtr String(int depth) {
    (void)depth;
    return rng_.Chance(70) ? MakeColumn(kStringColumn, kStr) : Literal(RandomScalar(kStr));
  }

  ExprPtr BoolLeaf() {
    return rng_.Chance(75) ? MakeColumn(0, kBool) : Literal(RandomScalar(kBool));
  }

  ExprPtr Bool(int depth) {
    if (depth <= 0 || rng_.Chance(10)) return BoolLeaf();
    const int64_t pick = rng_.Between(0, 11);
    const auto leaf = [&] { return BoolLeaf(); };
    switch (pick) {
      case 0:
        return Take(MakeNot(Bool(depth - 1)), leaf);
      case 1:
      case 2: {
        const BinaryOp op = rng_.Chance(50) ? BinaryOp::kAnd : BinaryOp::kOr;
        return Take(MakeBinary(op, Bool(depth - 1), Bool(depth - 1)), leaf);
      }
      case 3:
      case 4:
      case 5:
      case 6: {
        static const std::vector<BinaryOp> ops = {BinaryOp::kEq, BinaryOp::kNe, BinaryOp::kLt,
                                                  BinaryOp::kLe, BinaryOp::kGt, BinaryOp::kGe};
        const Category category = AnyCategory();
        return Take(MakeBinary(rng_.Pick(ops), Generate(category, depth - 1),
                               Generate(category, depth - 1)),
                    leaf);
      }
      case 7:
        return Take(MakeIsNull(Generate(AnyCategory(), depth - 1), rng_.Chance(50)), leaf);
      case 8:
      case 9: {
        const Category category = static_cast<Category>(rng_.Between(1, 3));
        return Take(MakeBetween(Generate(category, depth - 1), Generate(category, depth - 1),
                                Generate(category, depth - 1)),
                    leaf);
      }
      default: {
        const Category category = static_cast<Category>(rng_.Between(1, 3));
        ExprPtr operand = Generate(category, depth - 1);
        std::vector<std::shared_ptr<arrow::Scalar>> list;
        const int64_t size = rng_.Between(1, 4);
        for (int64_t i = 0; i < size; ++i) {
          auto scalar = category == Category::kNumeric    ? NumberScalar()
                        : category == Category::kCalendar ? RandomScalar(kDate)
                                                          : RandomScalar(kStr);
          list.push_back(std::move(scalar));
        }
        ExprPtr in_list = Take(MakeInList(std::move(operand), std::move(list)), leaf);
        return rng_.Chance(30) ? Take(MakeNot(std::move(in_list)), leaf) : std::move(in_list);
      }
    }
  }

  std::shared_ptr<arrow::Scalar> NumberScalar() {
    switch (rng_.Between(0, 3)) {
      case 0:
        return RandomScalar(kInt);
      case 1:
        return RandomScalar(kBig);
      case 2:
        return RandomScalar(kDbl);
      default: {
        const ExprPtr literal = DecimalLiteral();
        return literal->literal;
      }
    }
  }

  Random& rng_;
};

void CheckSeed(uint64_t seed, int* errors) {
  Random rng(seed);
  RandomBatch random = MakeRandomBatch(rng, seed);
  ExpressionGenerator generator(rng);
  const int64_t root = rng.Between(0, 9);
  const Category category = root <= 5   ? Category::kLogical
                            : root <= 7 ? Category::kNumeric
                            : root == 8 ? Category::kCalendar
                                        : Category::kText;
  const ExprPtr expr = generator.Generate(category, 4);
  SCOPED_TRACE("seed " + std::to_string(seed) + ", expression " + expr->ToString() + ", rows " +
               std::to_string(random.active_rows.size()));

  const auto expected = test::ReferenceEvaluate(*expr, random.active_rows);
  const auto actual = Evaluate(*expr, random.batch, arrow::default_memory_pool());
  if (!expected.ok()) {
    // Both must refuse, and for the same kind of reason.
    ASSERT_FALSE(actual.ok()) << "the reference failed (" << expected.status().ToString()
                              << ") but Evaluate returned rows";
    EXPECT_THAT(actual.status().message(), HasSubstr("overflow"));
    ++*errors;
    return;
  }
  ASSERT_TRUE(actual.ok()) << actual.status().ToString();
  ASSERT_TRUE((*actual)->type()->Equals(*expr->type.ToArrow()))
      << (*actual)->type()->ToString() << " vs " << expr->type.ToString();
  ASSERT_TRUE((*actual)->ValidateFull().ok());
  const auto values = test::ArrayToValues(**actual).ValueOrDie();
  ASSERT_EQ(values.size(), expected->size());
  for (size_t i = 0; i < values.size(); ++i) {
    ASSERT_TRUE(test::ValuesEqual(values[i], (*expected)[i]))
        << "row " << i << ": vectorized " << test::ValueToString(values[i]) << ", reference "
        << test::ValueToString((*expected)[i]);
  }

  if (category == Category::kLogical) {
    const auto selection = EvaluatePredicate(*expr, random.batch, arrow::default_memory_pool());
    ASSERT_TRUE(selection.ok()) << selection.status().ToString();
    std::vector<uint32_t> want;
    for (size_t i = 0; i < expected->size(); ++i) {
      const Value& v = (*expected)[i];
      if (std::holds_alternative<bool>(v) && std::get<bool>(v)) {
        want.push_back(random.batch.selection ? random.batch.selection->indices()[i]
                                              : static_cast<uint32_t>(i));
      }
    }
    EXPECT_EQ(std::vector<uint32_t>(selection->indices().begin(), selection->indices().end()),
              want);
  }
}

TEST(EvalPropertyTest, VectorizedEqualsReference) {
  int errors = 0;
  constexpr int kSeeds = 500;
  for (uint64_t seed = 0; seed < kSeeds; ++seed) {
    CheckSeed(seed, &errors);
    if (::testing::Test::HasFailure()) {
      break;  // the first failing seed is the one to debug
    }
  }
  // The property is vacuous if most random expressions overflow and are skipped.
  EXPECT_LT(errors, kSeeds / 4) << errors << " of " << kSeeds << " seeds ended in an overflow";
  RecordProperty("overflow_seeds", errors);
}

}  // namespace
}  // namespace quarry
