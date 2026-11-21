// Comparison kernels: = <> < <= > >= over two arrays of one type, with a BOOLEAN result that is
// NULL wherever either input is NULL.

#include <arrow/status.h>

#include <string_view>

#include "expr/eval_internal.h"

namespace quarry::eval_internal {
namespace {

// Sets bit i of `out` for every valid row where predicate(i) holds. `out` starts zeroed.
template <typename Predicate>
void FillBits(int64_t n, const uint8_t* valid, uint8_t* out, Predicate&& predicate) {
  ForRows(n, valid, [&](int64_t i) {
    if (predicate(i)) {
      arrow::bit_util::SetBit(out, i);
    }
  });
}

template <typename GetL, typename GetR>
void CompareRows(BinaryOp op, int64_t n, const uint8_t* valid, uint8_t* out, GetL&& l, GetR&& r) {
  switch (op) {
    case BinaryOp::kEq:
      FillBits(n, valid, out, [&](int64_t i) { return l(i) == r(i); });
      break;
    case BinaryOp::kNe:
      FillBits(n, valid, out, [&](int64_t i) { return l(i) != r(i); });
      break;
    case BinaryOp::kLt:
      FillBits(n, valid, out, [&](int64_t i) { return l(i) < r(i); });
      break;
    case BinaryOp::kLe:
      FillBits(n, valid, out, [&](int64_t i) { return l(i) <= r(i); });
      break;
    case BinaryOp::kGt:
      FillBits(n, valid, out, [&](int64_t i) { return l(i) > r(i); });
      break;
    default:
      FillBits(n, valid, out, [&](int64_t i) { return l(i) >= r(i); });
      break;
  }
}

template <typename T>
void CompareFixed(BinaryOp op, const arrow::Array& l, const arrow::Array& r, int64_t n,
                  const uint8_t* valid, uint8_t* out) {
  const uint8_t* a = ValueBytes(l);
  const uint8_t* b = ValueBytes(r);
  const int64_t a_offset = l.offset();
  const int64_t b_offset = r.offset();
  CompareRows(
      op, n, valid, out, [&](int64_t i) { return Load<T>(a, a_offset, i); },
      [&](int64_t i) { return Load<T>(b, b_offset, i); });
}

void CompareBooleans(BinaryOp op, const arrow::Array& l, const arrow::Array& r, int64_t n,
                     const uint8_t* valid, uint8_t* out) {
  const uint8_t* a = ValueBytes(l);
  const uint8_t* b = ValueBytes(r);
  const int64_t a_offset = l.offset();
  const int64_t b_offset = r.offset();
  CompareRows(
      op, n, valid, out, [&](int64_t i) { return BitAt(a, a_offset + i); },
      [&](int64_t i) { return BitAt(b, b_offset + i); });
}

struct StringColumn {
  const uint8_t* offsets;
  const uint8_t* bytes;
  int64_t offset;

  explicit StringColumn(const arrow::Array& array)
      : offsets(ValueBytes(array)),
        bytes(array.data()->buffers[2] == nullptr ? nullptr : array.data()->buffers[2]->data()),
        offset(array.offset()) {}

  std::string_view operator()(int64_t i) const {
    const int32_t begin = Load<int32_t>(offsets, offset, i);
    const int32_t end = Load<int32_t>(offsets, offset, i + 1);
    return {reinterpret_cast<const char*>(bytes) + begin, static_cast<size_t>(end - begin)};
  }
};

}  // namespace

arrow::Result<ArrayPtr> EvalComparison(BinaryOp op, const arrow::Array& l, const arrow::Array& r,
                                       const LogicalType& type, arrow::MemoryPool* pool) {
  if (!IsComparison(op)) {
    return arrow::Status::Invalid("operator ", BinaryOpName(op), " is not a comparison");
  }
  if (l.length() != r.length()) {
    return arrow::Status::Invalid("comparison of arrays of ", l.length(), " and ", r.length(),
                                  " rows");
  }
  ARROW_RETURN_NOT_OK(CheckArrayType(l, type, "left operand of a comparison"));
  ARROW_RETURN_NOT_OK(CheckArrayType(r, type, "right operand of a comparison"));
  const int64_t n = l.length();
  if (n == 0) {
    return arrow::MakeEmptyArray(arrow::boolean());
  }
  ARROW_ASSIGN_OR_RAISE(auto validity, AndValidity(l, r, pool));
  ARROW_ASSIGN_OR_RAISE(auto bits, AllocateZeroedBitmap(n, pool));
  uint8_t* out = bits->mutable_data();
  const uint8_t* valid = BitsOrNull(validity);
  switch (type.id) {
    case TypeId::kBoolean:
      CompareBooleans(op, l, r, n, valid, out);
      break;
    case TypeId::kInt32:
    case TypeId::kDate:
      CompareFixed<int32_t>(op, l, r, n, valid, out);
      break;
    case TypeId::kInt64:
      CompareFixed<int64_t>(op, l, r, n, valid, out);
      break;
    case TypeId::kDouble:
      CompareFixed<double>(op, l, r, n, valid, out);
      break;
    case TypeId::kDecimal:
      if (type.is_decimal64()) {
        CompareFixed<int64_t>(op, l, r, n, valid, out);
      } else {
        CompareFixed<Int128>(op, l, r, n, valid, out);
      }
      break;
    case TypeId::kVarchar: {
      const StringColumn a(l);
      const StringColumn b(r);
      CompareRows(op, n, valid, out, a, b);
      break;
    }
  }
  return FinishArray(arrow::boolean(), n, std::move(validity), std::move(bits));
}

}  // namespace quarry::eval_internal
