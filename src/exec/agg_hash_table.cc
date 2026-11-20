#include "exec/agg_hash_table.h"

#include <arrow/array/builder_binary.h>
#include <arrow/status.h>
#include <arrow/util/bit_util.h>

#include <cstring>
#include <string>

#include "exec/key_encoding.h"
#include "expr/eval_internal.h"
#include "types/decimal.h"

namespace quarry {
namespace {

using ArrayPtr = std::shared_ptr<arrow::Array>;
using eval_internal::BitAt;
using eval_internal::FinishArray;
using eval_internal::Load;
using eval_internal::ValueBytes;

}  // namespace

// The state of one aggregate for every group, in group-id order.
class AggregateState {
 public:
  virtual ~AggregateState() = default;
  // Makes room for groups [0, groups).
  virtual void Resize(int64_t groups) = 0;
  // Folds in row i of `arg` (null for COUNT(*)) into group group_ids[i], for i in [0, n).
  virtual arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) = 0;
  virtual arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                         arrow::MemoryPool* pool) const = 0;
  virtual int64_t memory_bytes() const = 0;
  // The partial state of groups [first, first + count) as columns of PartialTypes(), and the
  // inverse: folds the partial states of `n` groups (one row of `partial` each) into group
  // group_ids[i]. Spilling writes the first and the merge phase reads it back through the second.
  virtual std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const = 0;
  virtual arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                             arrow::MemoryPool* pool) const = 0;
  virtual arrow::Status Merge(const uint32_t* group_ids,
                              const std::vector<const arrow::Array*>& partial, int64_t n) = 0;
};

namespace {

const std::shared_ptr<arrow::DataType>& Int128Partial() {
  static const std::shared_ptr<arrow::DataType> type = arrow::decimal128(38, 0);
  return type;
}

// An array of fixed-width values, with a validity bitmap built from `valid` (1 = a value).
template <typename T>
arrow::Result<ArrayPtr> MakeFixedArray(const std::shared_ptr<arrow::DataType>& type,
                                       const std::vector<T>& values,
                                       const std::vector<uint8_t>& valid, int64_t first,
                                       int64_t count, arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(auto data, eval_internal::AllocateValues<T>(count, pool));
  ARROW_ASSIGN_OR_RAISE(auto validity, arrow::AllocateEmptyBitmap(count, pool));
  for (int64_t i = 0; i < count; ++i) {
    const auto at = static_cast<size_t>(first + i);
    if (valid[at] != 0) {
      arrow::bit_util::SetBit(validity->mutable_data(), i);
      std::memcpy(data->mutable_data() + static_cast<size_t>(i) * sizeof(T), &values[at],
                  sizeof(T));
    }
  }
  return FinishArray(type, count, std::move(validity), std::move(data));
}

const uint8_t* ValidBits(const arrow::Array& array) {
  return array.null_count() > 0 ? array.null_bitmap_data() : nullptr;
}

bool IsValid(const uint8_t* valid, const arrow::Array& array, int64_t i) {
  return valid == nullptr || BitAt(valid, array.offset() + i);
}

class CountState final : public AggregateState {
 public:
  explicit CountState(bool star) : star_(star) {}
  void Resize(int64_t groups) override { counts_.resize(static_cast<size_t>(groups), 0); }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    if (star_ || arg == nullptr) {
      for (int64_t i = 0; i < n; ++i) ++counts_[group_ids[i]];
      return arrow::Status::OK();
    }
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (IsValid(valid, *arg, i)) ++counts_[group_ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    const std::vector<uint8_t> all(counts_.size(), 1);
    return MakeFixedArray<int64_t>(arrow::int64(), counts_, all, first, count, pool);
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(counts_.capacity() * sizeof(int64_t));
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {arrow::int64()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto counts, Finish(first, count, pool));
    return std::vector<ArrayPtr>{std::move(counts)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    const uint8_t* values = ValueBytes(*partial[0]);
    for (int64_t i = 0; i < n; ++i) {
      counts_[group_ids[i]] += Load<int64_t>(values, partial[0]->offset(), i);
    }
    return arrow::Status::OK();
  }

 private:
  bool star_;
  std::vector<int64_t> counts_;
};

// SUM of integers and DECIMALs: a 128-bit accumulator that must stay within 38 digits. T is the
// type of the input values.
template <typename T>
class SumState final : public AggregateState {
 public:
  SumState(const LogicalType& result) : result_(result) {}
  void Resize(int64_t groups) override {
    sums_.resize(static_cast<size_t>(groups), 0);
    has_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const uint8_t* values = ValueBytes(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      Int128& sum = sums_[group_ids[i]];
      Int128 next;
      if (__builtin_add_overflow(sum, static_cast<Int128>(Load<T>(values, arg->offset(), i)),
                                 &next) ||
          !FitsPrecision<Int128>(next, kMaxDecimalPrecision)) {
        return arrow::Status::Invalid("decimal overflow in SUM: the sum does not fit DECIMAL(",
                                      kMaxDecimalPrecision, ")");
      }
      sum = next;
      has_[group_ids[i]] = 1;
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    return MakeFixedArray<Int128>(result_.ToArrow(), sums_, has_, first, count, pool);
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(sums_.capacity() * sizeof(Int128) + has_.capacity());
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {result_.ToArrow()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto sums, Finish(first, count, pool));
    return std::vector<ArrayPtr>{std::move(sums)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    const arrow::Array& arg = *partial[0];
    const uint8_t* values = ValueBytes(arg);
    const uint8_t* valid = ValidBits(arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, arg, i)) continue;
      Int128& sum = sums_[group_ids[i]];
      Int128 next;
      if (__builtin_add_overflow(sum, Load<Int128>(values, arg.offset(), i), &next) ||
          !FitsPrecision<Int128>(next, kMaxDecimalPrecision)) {
        return arrow::Status::Invalid("decimal overflow in SUM: the sum does not fit DECIMAL(",
                                      kMaxDecimalPrecision, ")");
      }
      sum = next;
      has_[group_ids[i]] = 1;
    }
    return arrow::Status::OK();
  }

 private:
  LogicalType result_;
  std::vector<Int128> sums_;
  std::vector<uint8_t> has_;
};

class SumDoubleState final : public AggregateState {
 public:
  void Resize(int64_t groups) override {
    sums_.resize(static_cast<size_t>(groups), 0.0);
    has_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const uint8_t* values = ValueBytes(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      sums_[group_ids[i]] += Load<double>(values, arg->offset(), i);
      has_[group_ids[i]] = 1;
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    return MakeFixedArray<double>(arrow::float64(), sums_, has_, first, count, pool);
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(sums_.capacity() * sizeof(double) + has_.capacity());
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {arrow::float64()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto sums, Finish(first, count, pool));
    return std::vector<ArrayPtr>{std::move(sums)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    return Update(group_ids, partial[0], n);  // a partial sum is folded in like a value
  }

 private:
  std::vector<double> sums_;
  std::vector<uint8_t> has_;
};

// AVG of integers and DECIMALs: the exact sum and the count, divided once at the end as a DOUBLE.
// A DECIMAL's scaled sum is divided by 10^scale and then by the count.
template <typename T>
class AvgState final : public AggregateState {
 public:
  explicit AvgState(int scale) : scale_(scale) {}
  void Resize(int64_t groups) override {
    sums_.resize(static_cast<size_t>(groups), 0);
    counts_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const uint8_t* values = ValueBytes(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      Int128& sum = sums_[group_ids[i]];
      Int128 next;
      if (__builtin_add_overflow(sum, static_cast<Int128>(Load<T>(values, arg->offset(), i)),
                                 &next)) {
        return arrow::Status::Invalid("overflow in AVG: the sum does not fit 128 bits");
      }
      sum = next;
      ++counts_[group_ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    std::vector<double> averages(sums_.size(), 0.0);
    std::vector<uint8_t> has(sums_.size(), 0);
    const double divisor = static_cast<double>(PowerOfTen<Int128>(scale_));
    for (size_t g = 0; g < sums_.size(); ++g) {
      if (counts_[g] == 0) continue;
      has[g] = 1;
      // The same order of operations as DuckDB, so that the last digit agrees: the sum as a
      // number first, then the mean.
      averages[g] = static_cast<double>(sums_[g]) / divisor / static_cast<double>(counts_[g]);
    }
    return MakeFixedArray<double>(arrow::float64(), averages, has, first, count, pool);
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(sums_.capacity() * sizeof(Int128) +
                                counts_.capacity() * sizeof(int64_t));
  }
  // The exact sum and the count, so that merging spilled partials divides once, at the end.
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {Int128Partial(), arrow::int64()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    const std::vector<uint8_t> all(sums_.size(), 1);
    ARROW_ASSIGN_OR_RAISE(auto sums,
                          MakeFixedArray<Int128>(Int128Partial(), sums_, all, first, count, pool));
    ARROW_ASSIGN_OR_RAISE(
        auto counts, MakeFixedArray<int64_t>(arrow::int64(), counts_, all, first, count, pool));
    return std::vector<ArrayPtr>{std::move(sums), std::move(counts)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    const uint8_t* sums = ValueBytes(*partial[0]);
    const uint8_t* counts = ValueBytes(*partial[1]);
    for (int64_t i = 0; i < n; ++i) {
      Int128 next;
      if (__builtin_add_overflow(sums_[group_ids[i]], Load<Int128>(sums, partial[0]->offset(), i),
                                 &next)) {
        return arrow::Status::Invalid("overflow in AVG: the sum does not fit 128 bits");
      }
      sums_[group_ids[i]] = next;
      counts_[group_ids[i]] += Load<int64_t>(counts, partial[1]->offset(), i);
    }
    return arrow::Status::OK();
  }

 private:
  int scale_;
  std::vector<Int128> sums_;
  std::vector<int64_t> counts_;
};

class AvgDoubleState final : public AggregateState {
 public:
  void Resize(int64_t groups) override {
    sums_.resize(static_cast<size_t>(groups), 0.0);
    counts_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const uint8_t* values = ValueBytes(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      sums_[group_ids[i]] += Load<double>(values, arg->offset(), i);
      ++counts_[group_ids[i]];
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    std::vector<double> averages(sums_.size(), 0.0);
    std::vector<uint8_t> has(sums_.size(), 0);
    for (size_t g = 0; g < sums_.size(); ++g) {
      if (counts_[g] == 0) continue;
      has[g] = 1;
      averages[g] = sums_[g] / static_cast<double>(counts_[g]);
    }
    return MakeFixedArray<double>(arrow::float64(), averages, has, first, count, pool);
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(sums_.capacity() * sizeof(double) +
                                counts_.capacity() * sizeof(int64_t));
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {arrow::float64(), arrow::int64()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    const std::vector<uint8_t> all(sums_.size(), 1);
    ARROW_ASSIGN_OR_RAISE(auto sums,
                          MakeFixedArray<double>(arrow::float64(), sums_, all, first, count, pool));
    ARROW_ASSIGN_OR_RAISE(
        auto counts, MakeFixedArray<int64_t>(arrow::int64(), counts_, all, first, count, pool));
    return std::vector<ArrayPtr>{std::move(sums), std::move(counts)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    const uint8_t* sums = ValueBytes(*partial[0]);
    const uint8_t* counts = ValueBytes(*partial[1]);
    for (int64_t i = 0; i < n; ++i) {
      sums_[group_ids[i]] += Load<double>(sums, partial[0]->offset(), i);
      counts_[group_ids[i]] += Load<int64_t>(counts, partial[1]->offset(), i);
    }
    return arrow::Status::OK();
  }

 private:
  std::vector<double> sums_;
  std::vector<int64_t> counts_;
};

// MIN and MAX of a fixed-width type. T is the stored value type.
template <typename T, bool kMax>
class MinMaxState final : public AggregateState {
 public:
  explicit MinMaxState(std::shared_ptr<arrow::DataType> type) : type_(std::move(type)) {}
  void Resize(int64_t groups) override {
    values_.resize(static_cast<size_t>(groups), T{});
    has_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const uint8_t* values = ValueBytes(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      const T value = Load<T>(values, arg->offset(), i);
      const uint32_t g = group_ids[i];
      if (has_[g] == 0 || (kMax ? value > values_[g] : value < values_[g])) {
        values_[g] = value;
        has_[g] = 1;
      }
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    return MakeFixedArray<T>(type_, values_, has_, first, count, pool);
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(values_.capacity() * sizeof(T) + has_.capacity());
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override { return {type_}; }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto values, Finish(first, count, pool));
    return std::vector<ArrayPtr>{std::move(values)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    return Update(group_ids, partial[0], n);  // the minimum of minima, the maximum of maxima
  }

 private:
  std::shared_ptr<arrow::DataType> type_;
  std::vector<T> values_;
  std::vector<uint8_t> has_;
};

template <bool kMax>
class MinMaxBoolState final : public AggregateState {
 public:
  void Resize(int64_t groups) override {
    values_.resize(static_cast<size_t>(groups), 0);
    has_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const uint8_t* bits = ValueBytes(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      const uint8_t value = BitAt(bits, arg->offset() + i) ? 1 : 0;
      const uint32_t g = group_ids[i];
      if (has_[g] == 0 || (kMax ? value > values_[g] : value < values_[g])) {
        values_[g] = value;
        has_[g] = 1;
      }
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto data, arrow::AllocateEmptyBitmap(count, pool));
    ARROW_ASSIGN_OR_RAISE(auto validity, arrow::AllocateEmptyBitmap(count, pool));
    for (int64_t i = 0; i < count; ++i) {
      const auto at = static_cast<size_t>(first + i);
      if (has_[at] == 0) continue;
      arrow::bit_util::SetBit(validity->mutable_data(), i);
      if (values_[at] != 0) arrow::bit_util::SetBit(data->mutable_data(), i);
    }
    return FinishArray(arrow::boolean(), count, std::move(validity), std::move(data));
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(values_.capacity() + has_.capacity());
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {arrow::boolean()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto values, Finish(first, count, pool));
    return std::vector<ArrayPtr>{std::move(values)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    return Update(group_ids, partial[0], n);  // the minimum of minima, the maximum of maxima
  }

 private:
  std::vector<uint8_t> values_;
  std::vector<uint8_t> has_;
};

template <bool kMax>
class MinMaxStringState final : public AggregateState {
 public:
  void Resize(int64_t groups) override {
    values_.resize(static_cast<size_t>(groups));
    has_.resize(static_cast<size_t>(groups), 0);
  }
  arrow::Status Update(const uint32_t* group_ids, const arrow::Array* arg, int64_t n) override {
    const auto& strings = static_cast<const arrow::StringArray&>(*arg);
    const uint8_t* valid = ValidBits(*arg);
    for (int64_t i = 0; i < n; ++i) {
      if (!IsValid(valid, *arg, i)) continue;
      const std::string_view value = strings.GetView(i);
      const uint32_t g = group_ids[i];
      if (has_[g] == 0 || (kMax ? value > values_[g] : value < values_[g])) {
        bytes_ += static_cast<int64_t>(value.size()) - static_cast<int64_t>(values_[g].size());
        values_[g].assign(value);
        has_[g] = 1;
      }
    }
    return arrow::Status::OK();
  }
  arrow::Result<ArrayPtr> Finish(int64_t first, int64_t count,
                                 arrow::MemoryPool* pool) const override {
    arrow::StringBuilder builder(pool);
    for (int64_t i = 0; i < count; ++i) {
      const auto at = static_cast<size_t>(first + i);
      if (has_[at] == 0) {
        ARROW_RETURN_NOT_OK(builder.AppendNull());
      } else {
        ARROW_RETURN_NOT_OK(builder.Append(values_[at]));
      }
    }
    ArrayPtr out;
    ARROW_RETURN_NOT_OK(builder.Finish(&out));
    return out;
  }
  int64_t memory_bytes() const override {
    return static_cast<int64_t>(values_.capacity() * sizeof(std::string) + has_.capacity()) +
           bytes_;
  }
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const override {
    return {arrow::utf8()};
  }
  arrow::Result<std::vector<ArrayPtr>> FinishPartial(int64_t first, int64_t count,
                                                     arrow::MemoryPool* pool) const override {
    ARROW_ASSIGN_OR_RAISE(auto values, Finish(first, count, pool));
    return std::vector<ArrayPtr>{std::move(values)};
  }
  arrow::Status Merge(const uint32_t* group_ids, const std::vector<const arrow::Array*>& partial,
                      int64_t n) override {
    return Update(group_ids, partial[0], n);  // the minimum of minima, the maximum of maxima
  }

 private:
  std::vector<std::string> values_;
  std::vector<uint8_t> has_;
  int64_t bytes_ = 0;
};

template <bool kMax>
arrow::Result<std::unique_ptr<AggregateState>> MakeMinMax(const LogicalType& type) {
  const auto arrow_type = type.ToArrow();
  switch (type.id) {
    case TypeId::kBoolean:
      return std::unique_ptr<AggregateState>(std::make_unique<MinMaxBoolState<kMax>>());
    case TypeId::kInt32:
    case TypeId::kDate:
      return std::unique_ptr<AggregateState>(
          std::make_unique<MinMaxState<int32_t, kMax>>(arrow_type));
    case TypeId::kInt64:
      return std::unique_ptr<AggregateState>(
          std::make_unique<MinMaxState<int64_t, kMax>>(arrow_type));
    case TypeId::kDecimal:
      if (type.is_decimal64()) {
        return std::unique_ptr<AggregateState>(
            std::make_unique<MinMaxState<int64_t, kMax>>(arrow_type));
      }
      return std::unique_ptr<AggregateState>(
          std::make_unique<MinMaxState<Int128, kMax>>(arrow_type));
    case TypeId::kDouble:
      return std::unique_ptr<AggregateState>(
          std::make_unique<MinMaxState<double, kMax>>(arrow_type));
    case TypeId::kVarchar:
      return std::unique_ptr<AggregateState>(std::make_unique<MinMaxStringState<kMax>>());
  }
  return arrow::Status::Invalid("MIN/MAX of ", type.ToString(), " is not supported");
}

arrow::Result<std::unique_ptr<AggregateState>> MakeState(const AggSpec& spec) {
  const LogicalType& arg = spec.arg_type;
  switch (spec.func) {
    case AggFunc::kCountStar:
      return std::unique_ptr<AggregateState>(std::make_unique<CountState>(true));
    case AggFunc::kCount:
      return std::unique_ptr<AggregateState>(std::make_unique<CountState>(false));
    case AggFunc::kMin:
      return MakeMinMax<false>(arg);
    case AggFunc::kMax:
      return MakeMinMax<true>(arg);
    case AggFunc::kSum:
      if (arg.id == TypeId::kInt32) {
        return std::unique_ptr<AggregateState>(
            std::make_unique<SumState<int32_t>>(spec.result_type));
      }
      if (arg.id == TypeId::kInt64 || arg.is_decimal64()) {
        return std::unique_ptr<AggregateState>(
            std::make_unique<SumState<int64_t>>(spec.result_type));
      }
      if (arg.is_decimal()) {
        return std::unique_ptr<AggregateState>(
            std::make_unique<SumState<Int128>>(spec.result_type));
      }
      if (arg.id == TypeId::kDouble) {
        return std::unique_ptr<AggregateState>(std::make_unique<SumDoubleState>());
      }
      break;
    case AggFunc::kAvg:
      if (arg.id == TypeId::kInt32) {
        return std::unique_ptr<AggregateState>(std::make_unique<AvgState<int32_t>>(0));
      }
      if (arg.id == TypeId::kInt64) {
        return std::unique_ptr<AggregateState>(std::make_unique<AvgState<int64_t>>(0));
      }
      if (arg.is_decimal64()) {
        return std::unique_ptr<AggregateState>(std::make_unique<AvgState<int64_t>>(arg.scale));
      }
      if (arg.is_decimal()) {
        return std::unique_ptr<AggregateState>(std::make_unique<AvgState<Int128>>(arg.scale));
      }
      if (arg.id == TypeId::kDouble) {
        return std::unique_ptr<AggregateState>(std::make_unique<AvgDoubleState>());
      }
      break;
  }
  return arrow::Status::Invalid(AggFuncName(spec.func), " of ", arg.ToString(),
                                " is not supported");
}

}  // namespace

AggHashTable::AggHashTable(std::vector<LogicalType> key_types, std::vector<AggSpec> aggregates,
                           Reservation* reservation, KeyHasher hasher)
    : key_types_(std::move(key_types)),
      specs_(std::move(aggregates)),
      reservation_(reservation),
      hasher_(hasher),
      index_(std::move(hasher)) {
  MakeStates();
}

void AggHashTable::MakeStates() {
  states_.clear();
  for (const AggSpec& spec : specs_) {
    auto state = MakeState(spec);
    // A spec the binder accepted always has a state; a failure here is a programming error that
    // Update reports (states_ is shorter than specs_).
    if (state.ok()) states_.push_back(std::move(*state));
  }
}

void AggHashTable::Clear() {
  index_ = KeyIndex(hasher_);
  MakeStates();
  groups_ = 0;
  over_budget_ = false;
  if (reservation_ != nullptr && charged_ > 0) reservation_->Shrink(charged_);
  charged_ = 0;
}

uint64_t AggHashTable::KeyHash(int64_t group) const { return index_.Hash(index_.key(group)); }

AggHashTable::~AggHashTable() = default;

int64_t AggHashTable::num_groups() const { return groups_; }

int64_t AggHashTable::memory_bytes() const {
  int64_t bytes = index_.memory_bytes();
  for (const auto& state : states_) bytes += state->memory_bytes();
  return bytes;
}

arrow::Status AggHashTable::Charge() {
  if (reservation_ == nullptr) return arrow::Status::OK();
  const int64_t now = memory_bytes();
  if (now > charged_) {
    if (spillable_) {
      if (!reservation_->TryGrow(now - charged_)) {
        over_budget_ = true;  // the caller spills the table and clears it
        return arrow::Status::OK();
      }
    } else {
      ARROW_RETURN_NOT_OK(reservation_->Grow(now - charged_));
    }
    charged_ = now;
  }
  return arrow::Status::OK();
}

arrow::Status AggHashTable::FindGroups(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                       int64_t n, std::vector<uint32_t>* group_ids) {
  if (states_.size() != specs_.size()) {
    return arrow::Status::Invalid("an aggregate of the plan has no implementation");
  }
  if (keys.size() != key_types_.size()) {
    return arrow::Status::Invalid("aggregate update has ", keys.size(), " keys, expected ",
                                  key_types_.size());
  }
  group_ids->assign(static_cast<size_t>(n), 0);
  if (key_types_.empty()) {
    if (n > 0) {
      ARROW_RETURN_NOT_OK(EnsureGlobalGroup());
    }
  } else {
    EncodedKeys encoded;
    ARROW_RETURN_NOT_OK(EncodeKeys(keys, n, &encoded));
    for (int64_t i = 0; i < n; ++i) {
      const std::span<const uint8_t> key = encoded.row(i);
      const uint64_t hash = index_.Hash(key);
      int64_t id = index_.Find(key, hash);
      if (id == KeyIndex::kAbsent) {
        id = index_.Insert(key, hash);
        ++groups_;
      }
      (*group_ids)[static_cast<size_t>(i)] = static_cast<uint32_t>(id);
    }
  }
  if (groups_ > static_cast<int64_t>(UINT32_MAX)) {
    return arrow::Status::Invalid("more than 2^32 groups");
  }
  for (const auto& state : states_) state->Resize(groups_);
  return arrow::Status::OK();
}

arrow::Status AggHashTable::Update(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                   const std::vector<std::shared_ptr<arrow::Array>>& args,
                                   int64_t n) {
  if (args.size() != specs_.size()) {
    return arrow::Status::Invalid("aggregate update has ", args.size(), " arguments, expected ",
                                  specs_.size());
  }
  std::vector<uint32_t> group_ids;
  ARROW_RETURN_NOT_OK(FindGroups(keys, n, &group_ids));
  if (n > 0) {
    for (size_t a = 0; a < states_.size(); ++a) {
      ARROW_RETURN_NOT_OK(states_[a]->Update(group_ids.data(), args[a].get(), n));
    }
  }
  return Charge();
}

std::vector<std::shared_ptr<arrow::DataType>> AggHashTable::PartialTypes() const {
  std::vector<std::shared_ptr<arrow::DataType>> types;
  for (const LogicalType& t : key_types_) types.push_back(t.ToArrow());
  for (const auto& state : states_) {
    for (auto& type : state->PartialTypes()) types.push_back(std::move(type));
  }
  return types;
}

arrow::Status AggHashTable::MergePartial(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                         const std::vector<std::shared_ptr<arrow::Array>>& partials,
                                         int64_t n) {
  std::vector<uint32_t> group_ids;
  ARROW_RETURN_NOT_OK(FindGroups(keys, n, &group_ids));
  size_t next = 0;
  for (const auto& state : states_) {
    const size_t width = state->PartialTypes().size();
    if (next + width > partials.size()) {
      return arrow::Status::Invalid("a spilled aggregate row has too few partial columns");
    }
    std::vector<const arrow::Array*> columns;
    for (size_t i = 0; i < width; ++i) columns.push_back(partials[next + i].get());
    next += width;
    if (n > 0) ARROW_RETURN_NOT_OK(state->Merge(group_ids.data(), columns, n));
  }
  if (next != partials.size()) {
    return arrow::Status::Invalid("a spilled aggregate row has too many partial columns");
  }
  return Charge();
}

arrow::Status AggHashTable::EnsureGlobalGroup() {
  if (!key_types_.empty()) {
    return arrow::Status::Invalid("a table with key columns has no global group");
  }
  if (groups_ == 0) {
    groups_ = 1;
    for (const auto& state : states_) state->Resize(1);
  }
  return Charge();
}

arrow::Result<std::vector<std::shared_ptr<arrow::Array>>> AggHashTable::Finish(
    int64_t first, int64_t count, arrow::MemoryPool* pool) const {
  if (first < 0 || count < 0 || first + count > groups_) {
    return arrow::Status::Invalid("groups [", first, ", ", first + count,
                                  ") are out of range: ", groups_, " groups");
  }
  std::vector<ArrayPtr> out;
  if (!key_types_.empty()) {
    std::vector<std::shared_ptr<arrow::DataType>> types;
    types.reserve(key_types_.size());
    for (const LogicalType& t : key_types_) types.push_back(t.ToArrow());
    std::vector<std::span<const uint8_t>> rows;
    rows.reserve(static_cast<size_t>(count));
    for (int64_t g = first; g < first + count; ++g) rows.push_back(index_.key(g));
    ARROW_ASSIGN_OR_RAISE(out, DecodeKeys(types, rows, pool));
  }
  for (const auto& state : states_) {
    ARROW_ASSIGN_OR_RAISE(auto column, state->Finish(first, count, pool));
    out.push_back(std::move(column));
  }
  return out;
}

arrow::Result<std::vector<std::shared_ptr<arrow::Array>>> AggHashTable::FinishPartial(
    int64_t first, int64_t count, arrow::MemoryPool* pool) const {
  if (first < 0 || count < 0 || first + count > groups_) {
    return arrow::Status::Invalid("groups [", first, ", ", first + count,
                                  ") are out of range: ", groups_, " groups");
  }
  std::vector<ArrayPtr> out;
  if (!key_types_.empty()) {
    std::vector<std::shared_ptr<arrow::DataType>> types;
    types.reserve(key_types_.size());
    for (const LogicalType& t : key_types_) types.push_back(t.ToArrow());
    std::vector<std::span<const uint8_t>> rows;
    rows.reserve(static_cast<size_t>(count));
    for (int64_t g = first; g < first + count; ++g) rows.push_back(index_.key(g));
    ARROW_ASSIGN_OR_RAISE(out, DecodeKeys(types, rows, pool));
  }
  for (const auto& state : states_) {
    ARROW_ASSIGN_OR_RAISE(auto columns, state->FinishPartial(first, count, pool));
    for (auto& column : columns) out.push_back(std::move(column));
  }
  return out;
}

}  // namespace quarry
