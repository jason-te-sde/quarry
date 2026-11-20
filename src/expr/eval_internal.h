#ifndef QUARRY_EXPR_EVAL_INTERNAL_H_
#define QUARRY_EXPR_EVAL_INTERNAL_H_

// The kernels behind Evaluate and the helpers they share. Each kernel is a typed loop over whole
// columns; the operator and the physical type are template parameters, so no loop makes a call
// through a pointer or a switch per row. Every kernel takes arrays that are already of the exact
// types the expression tree promises (the factories of bound_expr.h insert the casts) and returns
// Invalid when they are not.

#include <arrow/array.h>
#include <arrow/buffer.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/util/bit_util.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include "types/decimal.h"
#include "types/logical_type.h"
#include "types/type_rules.h"

namespace quarry::eval_internal {

using ArrayPtr = std::shared_ptr<arrow::Array>;
using BufferPtr = std::shared_ptr<arrow::Buffer>;

// --- helpers ---------------------------------------------------------------------------------

// The values of a fixed-width array, at its own offset. Never read an element through the pointer:
// use Load, which does not assume alignment.
inline const uint8_t* ValueBytes(const arrow::Array& array) {
  const auto& buffer = array.data()->buffers[1];
  return buffer == nullptr ? nullptr : buffer->data();
}

template <typename T>
inline T Load(const uint8_t* base, int64_t offset, int64_t i) {
  T value;
  std::memcpy(&value, base + static_cast<size_t>(offset + i) * sizeof(T), sizeof(T));
  return value;
}

inline bool BitAt(const uint8_t* bits, int64_t i) {
  return arrow::bit_util::GetBit(bits, static_cast<uint64_t>(i));
}

// Calls f(i) for every row, or only for the rows whose bit is set when `valid` is not null.
template <typename F>
inline void ForRows(int64_t n, const uint8_t* valid, F&& f) {
  if (valid == nullptr) {
    for (int64_t i = 0; i < n; ++i) {
      f(i);
    }
  } else {
    for (int64_t i = 0; i < n; ++i) {
      if (BitAt(valid, i)) {
        f(i);
      }
    }
  }
}

// A zero-filled buffer of n values of type T.
template <typename T>
arrow::Result<BufferPtr> AllocateValues(int64_t n, arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(auto buffer,
                        arrow::AllocateBuffer(n * static_cast<int64_t>(sizeof(T)), pool));
  if (buffer->size() > 0) {
    std::memset(buffer->mutable_data(), 0, static_cast<size_t>(buffer->size()));
  }
  return BufferPtr(std::move(buffer));
}

// A zero-filled bitmap of n bits.
arrow::Result<BufferPtr> AllocateZeroedBitmap(int64_t n, arrow::MemoryPool* pool);

// The validity of a row-wise function of one or two inputs: nullptr when no input has a NULL,
// otherwise a bitmap at offset 0 (a row is valid when it is valid in every input).
arrow::Result<BufferPtr> CopyValidity(const arrow::Array& a, arrow::MemoryPool* pool);
arrow::Result<BufferPtr> AndValidity(const arrow::Array& a, const arrow::Array& b,
                                     arrow::MemoryPool* pool);
inline const uint8_t* BitsOrNull(const BufferPtr& buffer) {
  return buffer == nullptr ? nullptr : buffer->data();
}

// An array of `type` from a values buffer (or bitmap, for BOOLEAN) and a validity bitmap that may
// be null; the null count is counted from the bitmap, and an all-valid bitmap is dropped.
ArrayPtr FinishArray(const std::shared_ptr<arrow::DataType>& type, int64_t n, BufferPtr validity,
                     BufferPtr values);

// An Invalid status unless the array has the Arrow type of `type`.
arrow::Status CheckArrayType(const arrow::Array& array, const LogicalType& type, const char* what);

// --- kernels ---------------------------------------------------------------------------------

// l op r for + - *. `result` is the type of the output; for DECIMAL add and subtract both inputs
// have the result type, for DECIMAL multiply they are DECIMALs stored in the width of the result.
arrow::Result<ArrayPtr> EvalArithmetic(BinaryOp op, const arrow::Array& l, const LogicalType& lt,
                                       const arrow::Array& r, const LogicalType& rt,
                                       const LogicalType& result, arrow::MemoryPool* pool);

arrow::Result<ArrayPtr> EvalNegate(const arrow::Array& input, const LogicalType& type,
                                   arrow::MemoryPool* pool);

// = <> < <= > >=; both inputs have type `type`.
arrow::Result<ArrayPtr> EvalComparison(BinaryOp op, const arrow::Array& l, const arrow::Array& r,
                                       const LogicalType& type, arrow::MemoryPool* pool);

// Three-valued AND and OR of two BOOLEAN arrays.
arrow::Result<ArrayPtr> EvalLogical(BinaryOp op, const arrow::Array& l, const arrow::Array& r,
                                    arrow::MemoryPool* pool);
arrow::Result<ArrayPtr> EvalNot(const arrow::Array& input, arrow::MemoryPool* pool);
arrow::Result<ArrayPtr> EvalIsNull(const arrow::Array& input, bool negated,
                                   arrow::MemoryPool* pool);

arrow::Result<ArrayPtr> EvalCast(const arrow::Array& input, const LogicalType& from,
                                 const LogicalType& to, arrow::MemoryPool* pool);

// A decimal128 array whose precision is <= 18 as the decimal64 array of the same values.
arrow::Result<ArrayPtr> NarrowDecimal128(const arrow::Array& input, arrow::MemoryPool* pool);

}  // namespace quarry::eval_internal

#endif  // QUARRY_EXPR_EVAL_INTERNAL_H_
