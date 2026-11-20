// CAST kernels: INTEGER -> BIGINT, DOUBLE, DECIMAL; BIGINT -> DOUBLE, DECIMAL; DECIMAL -> DECIMAL
// (rescale, rounding half away from zero) and DOUBLE.

#include <arrow/status.h>

#include "expr/eval_internal.h"

namespace quarry::eval_internal {
namespace {

// Applies f(in, &out) to every valid row; f returns false on overflow. Rows that are NULL are not
// passed to f.
template <typename In, typename Out, typename F>
arrow::Result<ArrayPtr> MapChecked(const arrow::Array& input, const LogicalType& to, F&& f,
                                   arrow::MemoryPool* pool) {
  const int64_t n = input.length();
  ARROW_ASSIGN_OR_RAISE(auto validity, CopyValidity(input, pool));
  ARROW_ASSIGN_OR_RAISE(auto values, AllocateValues<Out>(n, pool));
  Out* out = reinterpret_cast<Out*>(values->mutable_data());
  const uint8_t* in = ValueBytes(input);
  const int64_t offset = input.offset();
  bool overflow = false;
  ForRows(n, BitsOrNull(validity), [&](int64_t i) {
    Out result{};
    if (f(Load<In>(in, offset, i), &result)) {
      out[i] = result;
    } else {
      overflow = true;
    }
  });
  if (overflow) {
    return arrow::Status::Invalid("decimal overflow in cast to ", to.ToString());
  }
  return FinishArray(to.ToArrow(), n, std::move(validity), std::move(values));
}

template <typename In, typename Out>
arrow::Result<ArrayPtr> Convert(const arrow::Array& input, const LogicalType& to,
                                arrow::MemoryPool* pool) {
  return MapChecked<In, Out>(
      input, to,
      [](In v, Out* out) {
        *out = static_cast<Out>(v);
        return true;
      },
      pool);
}

// An integer to DECIMAL(p, s): the value times 10^s must fit p digits. W is the width the
// arithmetic runs in, the width of the target.
template <typename In, typename Out>
arrow::Result<ArrayPtr> IntegerToDecimal(const arrow::Array& input, const LogicalType& to,
                                         arrow::MemoryPool* pool) {
  return MapChecked<In, Out>(
      input, to,
      [&to](In v, Out* out) {
        Out scaled;
        if (!RescaleValue<Out>(static_cast<Out>(v), 0, to.scale, &scaled) ||
            !FitsPrecision<Out>(scaled, to.precision)) {
          return false;
        }
        *out = scaled;
        return true;
      },
      pool);
}

// DECIMAL -> DECIMAL. W is the wider of the two storage widths.
template <typename In, typename Out, typename W>
arrow::Result<ArrayPtr> RescaleDecimal(const arrow::Array& input, const LogicalType& from,
                                       const LogicalType& to, arrow::MemoryPool* pool) {
  return MapChecked<In, Out>(
      input, to,
      [&from, &to](In v, Out* out) {
        W scaled;
        if (!RescaleValue<W>(static_cast<W>(v), from.scale, to.scale, &scaled) ||
            !FitsPrecision<W>(scaled, to.precision)) {
          return false;
        }
        *out = static_cast<Out>(scaled);
        return true;
      },
      pool);
}

template <typename In>
arrow::Result<ArrayPtr> DecimalToDoubleArray(const arrow::Array& input, const LogicalType& from,
                                             arrow::MemoryPool* pool) {
  const int scale = from.scale;
  return MapChecked<In, double>(
      input, LogicalType::Double(),
      [scale](In v, double* out) {
        *out = DecimalToDouble(static_cast<Int128>(v), scale);
        return true;
      },
      pool);
}

}  // namespace

arrow::Result<ArrayPtr> EvalCast(const arrow::Array& input, const LogicalType& from,
                                 const LogicalType& to, arrow::MemoryPool* pool) {
  ARROW_RETURN_NOT_OK(CheckArrayType(input, from, "operand of a cast"));
  if (from == to) {
    return arrow::MakeArray(input.data());
  }
  if (input.length() == 0) {
    return arrow::MakeEmptyArray(to.ToArrow());
  }
  const bool from_int32 = from.id == TypeId::kInt32;
  const bool from_int64 = from.id == TypeId::kInt64;
  switch (to.id) {
    case TypeId::kInt64:
      if (from_int32) return Convert<int32_t, int64_t>(input, to, pool);
      break;
    case TypeId::kDouble:
      if (from_int32) return Convert<int32_t, double>(input, to, pool);
      if (from_int64) return Convert<int64_t, double>(input, to, pool);
      if (from.is_decimal64()) return DecimalToDoubleArray<int64_t>(input, from, pool);
      if (from.is_decimal()) return DecimalToDoubleArray<Int128>(input, from, pool);
      break;
    case TypeId::kDecimal:
      if (from_int32 || from_int64) {
        if (to.is_decimal64()) {
          return from_int32 ? IntegerToDecimal<int32_t, int64_t>(input, to, pool)
                            : IntegerToDecimal<int64_t, int64_t>(input, to, pool);
        }
        return from_int32 ? IntegerToDecimal<int32_t, Int128>(input, to, pool)
                          : IntegerToDecimal<int64_t, Int128>(input, to, pool);
      }
      if (from.is_decimal()) {
        if (from.is_decimal64() && to.is_decimal64()) {
          return RescaleDecimal<int64_t, int64_t, int64_t>(input, from, to, pool);
        }
        if (from.is_decimal64()) {
          return RescaleDecimal<int64_t, Int128, Int128>(input, from, to, pool);
        }
        if (to.is_decimal64()) {
          return RescaleDecimal<Int128, int64_t, Int128>(input, from, to, pool);
        }
        return RescaleDecimal<Int128, Int128, Int128>(input, from, to, pool);
      }
      break;
    default:
      break;
  }
  return arrow::Status::Invalid("cannot cast ", from.ToString(), " to ", to.ToString());
}

arrow::Result<ArrayPtr> NarrowDecimal128(const arrow::Array& input, arrow::MemoryPool* pool) {
  if (input.type_id() != arrow::Type::DECIMAL128) {
    return arrow::Status::Invalid("expected a decimal128 array, found ", input.type()->ToString());
  }
  const auto& type = static_cast<const arrow::Decimal128Type&>(*input.type());
  if (type.precision() > kMaxInt64DecimalPrecision) {
    return arrow::Status::Invalid("decimal128 precision ", type.precision(),
                                  " does not fit 64 bits");
  }
  const auto to = LogicalType::Decimal(type.precision(), type.scale());
  return MapChecked<Int128, int64_t>(
      input, to,
      [&to](Int128 v, int64_t* out) {
        if (!FitsPrecision<Int128>(v, to.precision)) {
          return false;
        }
        *out = static_cast<int64_t>(v);
        return true;
      },
      pool);
}

}  // namespace quarry::eval_internal
