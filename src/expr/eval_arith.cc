// Arithmetic kernels: + - * on integers, doubles, DECIMALs and DATE +/- INTEGER, and unary minus.

#include <arrow/status.h>

#include "expr/eval_internal.h"

namespace quarry::eval_internal {
namespace {

template <BinaryOp kOp, typename T>
inline bool ApplyChecked(T a, T b, T* out) {
  if constexpr (kOp == BinaryOp::kAdd) {
    return __builtin_add_overflow(a, b, out);
  } else if constexpr (kOp == BinaryOp::kSub) {
    return __builtin_sub_overflow(a, b, out);
  } else {
    return __builtin_mul_overflow(a, b, out);
  }
}

// out[i] = a[i] op b[i] for the valid rows. precision < 0 means "any value of T"; otherwise the
// result must also fit that many decimal digits. Returns false when a valid row overflowed.
template <typename T, BinaryOp kOp>
bool CheckedLoop(const arrow::Array& l, const arrow::Array& r, T* out, int64_t n,
                 const uint8_t* valid, int precision) {
  const uint8_t* a = ValueBytes(l);
  const uint8_t* b = ValueBytes(r);
  const int64_t a_offset = l.offset();
  const int64_t b_offset = r.offset();
  bool overflow = false;
  ForRows(n, valid, [&](int64_t i) {
    T result;
    bool bad = ApplyChecked<kOp, T>(Load<T>(a, a_offset, i), Load<T>(b, b_offset, i), &result);
    if (precision >= 0) {
      bad = bad || !FitsPrecision(result, precision);
    }
    overflow = overflow || bad;
    out[i] = bad ? T{} : result;
  });
  return !overflow;
}

template <BinaryOp kOp>
void DoubleLoop(const arrow::Array& l, const arrow::Array& r, double* out, int64_t n,
                const uint8_t* valid) {
  const uint8_t* a = ValueBytes(l);
  const uint8_t* b = ValueBytes(r);
  const int64_t a_offset = l.offset();
  const int64_t b_offset = r.offset();
  ForRows(n, valid, [&](int64_t i) {
    const double x = Load<double>(a, a_offset, i);
    const double y = Load<double>(b, b_offset, i);
    if constexpr (kOp == BinaryOp::kAdd) {
      out[i] = x + y;
    } else if constexpr (kOp == BinaryOp::kSub) {
      out[i] = x - y;
    } else {
      out[i] = x * y;
    }
  });
}

const char* OperationName(BinaryOp op) {
  switch (op) {
    case BinaryOp::kAdd:
      return "addition";
    case BinaryOp::kSub:
      return "subtraction";
    default:
      return "multiplication";
  }
}

// precision < 0: integer arithmetic; the status says "integer overflow". Otherwise "decimal
// overflow" and the precision is checked.
template <typename T>
arrow::Result<ArrayPtr> RunChecked(BinaryOp op, const std::shared_ptr<arrow::DataType>& type,
                                   const arrow::Array& l, const arrow::Array& r, int precision,
                                   arrow::MemoryPool* pool) {
  const int64_t n = l.length();
  ARROW_ASSIGN_OR_RAISE(auto validity, AndValidity(l, r, pool));
  ARROW_ASSIGN_OR_RAISE(auto values, AllocateValues<T>(n, pool));
  T* out = reinterpret_cast<T*>(values->mutable_data());
  const uint8_t* valid = BitsOrNull(validity);
  bool ok = false;
  switch (op) {
    case BinaryOp::kAdd:
      ok = CheckedLoop<T, BinaryOp::kAdd>(l, r, out, n, valid, precision);
      break;
    case BinaryOp::kSub:
      ok = CheckedLoop<T, BinaryOp::kSub>(l, r, out, n, valid, precision);
      break;
    default:
      ok = CheckedLoop<T, BinaryOp::kMul>(l, r, out, n, valid, precision);
      break;
  }
  if (!ok) {
    if (precision < 0) {
      return arrow::Status::Invalid("integer overflow in ", OperationName(op), " of ",
                                    type->ToString());
    }
    return DecimalOverflow(OperationName(op), precision);
  }
  return FinishArray(type, n, std::move(validity), std::move(values));
}

arrow::Result<ArrayPtr> RunDouble(BinaryOp op, const arrow::Array& l, const arrow::Array& r,
                                  arrow::MemoryPool* pool) {
  const int64_t n = l.length();
  ARROW_ASSIGN_OR_RAISE(auto validity, AndValidity(l, r, pool));
  ARROW_ASSIGN_OR_RAISE(auto values, AllocateValues<double>(n, pool));
  double* out = reinterpret_cast<double*>(values->mutable_data());
  const uint8_t* valid = BitsOrNull(validity);
  switch (op) {
    case BinaryOp::kAdd:
      DoubleLoop<BinaryOp::kAdd>(l, r, out, n, valid);
      break;
    case BinaryOp::kSub:
      DoubleLoop<BinaryOp::kSub>(l, r, out, n, valid);
      break;
    default:
      DoubleLoop<BinaryOp::kMul>(l, r, out, n, valid);
      break;
  }
  return FinishArray(arrow::float64(), n, std::move(validity), std::move(values));
}

template <typename T>
arrow::Result<ArrayPtr> RunNegate(const arrow::Array& input, const LogicalType& type, int precision,
                                  arrow::MemoryPool* pool) {
  const int64_t n = input.length();
  ARROW_ASSIGN_OR_RAISE(auto validity, CopyValidity(input, pool));
  ARROW_ASSIGN_OR_RAISE(auto values, AllocateValues<T>(n, pool));
  T* out = reinterpret_cast<T*>(values->mutable_data());
  const uint8_t* in = ValueBytes(input);
  const int64_t offset = input.offset();
  bool overflow = false;
  ForRows(n, BitsOrNull(validity), [&](int64_t i) {
    T result;
    const bool bad = __builtin_sub_overflow(T{0}, Load<T>(in, offset, i), &result);
    overflow = overflow || bad;
    out[i] = bad ? T{} : result;
  });
  if (overflow) {
    return arrow::Status::Invalid(precision < 0 ? "integer" : "decimal",
                                  " overflow in negation of ", type.ToString());
  }
  return FinishArray(type.ToArrow(), n, std::move(validity), std::move(values));
}

}  // namespace

arrow::Result<ArrayPtr> EvalArithmetic(BinaryOp op, const arrow::Array& l, const LogicalType& lt,
                                       const arrow::Array& r, const LogicalType& rt,
                                       const LogicalType& result, arrow::MemoryPool* pool) {
  if (l.length() != r.length()) {
    return arrow::Status::Invalid("arithmetic on arrays of ", l.length(), " and ", r.length(),
                                  " rows");
  }
  ARROW_RETURN_NOT_OK(CheckArrayType(l, lt, "left operand"));
  ARROW_RETURN_NOT_OK(CheckArrayType(r, rt, "right operand"));
  const auto mismatch = [&]() {
    return arrow::Status::Invalid("operator ", BinaryOpName(op), " on ", lt.ToString(), " and ",
                                  rt.ToString(), " does not produce ", result.ToString(),
                                  ": the operands need a cast");
  };
  if (l.length() == 0) {
    return arrow::MakeEmptyArray(result.ToArrow());
  }
  switch (result.id) {
    case TypeId::kInt32:
      if (lt != result || rt != result) return mismatch();
      return RunChecked<int32_t>(op, result.ToArrow(), l, r, -1, pool);
    case TypeId::kInt64:
      if (lt != result || rt != result) return mismatch();
      return RunChecked<int64_t>(op, result.ToArrow(), l, r, -1, pool);
    case TypeId::kDouble:
      if (lt != result || rt != result) return mismatch();
      return RunDouble(op, l, r, pool);
    case TypeId::kDate: {
      // DATE +/- INTEGER (or INTEGER + DATE): days arithmetic on the underlying int32.
      const bool date_int = lt.id == TypeId::kDate && rt.id == TypeId::kInt32;
      const bool int_date =
          lt.id == TypeId::kInt32 && rt.id == TypeId::kDate && op == BinaryOp::kAdd;
      if (op == BinaryOp::kMul || !(date_int || int_date)) return mismatch();
      return RunChecked<int32_t>(op, result.ToArrow(), l, r, -1, pool);
    }
    case TypeId::kDecimal: {
      if (!lt.is_decimal() || !rt.is_decimal()) return mismatch();
      if (op == BinaryOp::kMul) {
        if (lt.scale + rt.scale != result.scale || lt.is_decimal64() != result.is_decimal64() ||
            rt.is_decimal64() != result.is_decimal64()) {
          return mismatch();
        }
      } else if (lt != result || rt != result) {
        return mismatch();
      }
      if (result.is_decimal64()) {
        return RunChecked<int64_t>(op, result.ToArrow(), l, r, result.precision, pool);
      }
      return RunChecked<Int128>(op, result.ToArrow(), l, r, result.precision, pool);
    }
    default:
      return mismatch();
  }
}

arrow::Result<ArrayPtr> EvalNegate(const arrow::Array& input, const LogicalType& type,
                                   arrow::MemoryPool* pool) {
  ARROW_RETURN_NOT_OK(CheckArrayType(input, type, "operand of unary minus"));
  if (input.length() == 0) {
    return arrow::MakeEmptyArray(type.ToArrow());
  }
  switch (type.id) {
    case TypeId::kInt32:
      return RunNegate<int32_t>(input, type, -1, pool);
    case TypeId::kInt64:
      return RunNegate<int64_t>(input, type, -1, pool);
    case TypeId::kDecimal:
      if (type.is_decimal64()) {
        return RunNegate<int64_t>(input, type, type.precision, pool);
      }
      return RunNegate<Int128>(input, type, type.precision, pool);
    case TypeId::kDouble: {
      const int64_t n = input.length();
      ARROW_ASSIGN_OR_RAISE(auto validity, CopyValidity(input, pool));
      ARROW_ASSIGN_OR_RAISE(auto values, AllocateValues<double>(n, pool));
      double* out = reinterpret_cast<double*>(values->mutable_data());
      const uint8_t* in = ValueBytes(input);
      const int64_t offset = input.offset();
      ForRows(n, BitsOrNull(validity), [&](int64_t i) { out[i] = -Load<double>(in, offset, i); });
      return FinishArray(arrow::float64(), n, std::move(validity), std::move(values));
    }
    default:
      return arrow::Status::Invalid("unary minus cannot be applied to ", type.ToString());
  }
}

}  // namespace quarry::eval_internal
