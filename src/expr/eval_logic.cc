// Three-valued logic: AND, OR, NOT and IS [NOT] NULL over BOOLEAN arrays.

#include <arrow/status.h>

#include "common/flaws.h"
#include "expr/eval_internal.h"

namespace quarry::eval_internal {
namespace {

arrow::Status CheckBoolean(const arrow::Array& array, const char* what) {
  if (array.type_id() != arrow::Type::BOOL) {
    return arrow::Status::Invalid(what, " must be BOOLEAN but is ", array.type()->ToString());
  }
  return arrow::Status::OK();
}

}  // namespace

arrow::Result<ArrayPtr> EvalLogical(BinaryOp op, const arrow::Array& l, const arrow::Array& r,
                                    arrow::MemoryPool* pool) {
  if (op != BinaryOp::kAnd && op != BinaryOp::kOr) {
    return arrow::Status::Invalid("operator ", BinaryOpName(op), " is not AND or OR");
  }
  if (l.length() != r.length()) {
    return arrow::Status::Invalid(BinaryOpName(op), " of arrays of ", l.length(), " and ",
                                  r.length(), " rows");
  }
  ARROW_RETURN_NOT_OK(CheckBoolean(l, "left operand"));
  ARROW_RETURN_NOT_OK(CheckBoolean(r, "right operand"));
  const int64_t n = l.length();
  if (n == 0) {
    return arrow::MakeEmptyArray(arrow::boolean());
  }
  ARROW_ASSIGN_OR_RAISE(auto validity, AllocateZeroedBitmap(n, pool));
  ARROW_ASSIGN_OR_RAISE(auto bits, AllocateZeroedBitmap(n, pool));
  uint8_t* out_valid = validity->mutable_data();
  uint8_t* out_bits = bits->mutable_data();
  const uint8_t* lv = l.null_count() > 0 ? l.null_bitmap_data() : nullptr;
  const uint8_t* rv = r.null_count() > 0 ? r.null_bitmap_data() : nullptr;
  const uint8_t* lb = ValueBytes(l);
  const uint8_t* rb = ValueBytes(r);
  const bool is_and = op == BinaryOp::kAnd;
  for (int64_t i = 0; i < n; ++i) {
    const bool l_known = lv == nullptr || BitAt(lv, l.offset() + i);
    const bool r_known = rv == nullptr || BitAt(rv, r.offset() + i);
    const bool l_true = l_known && BitAt(lb, l.offset() + i);
    const bool r_true = r_known && BitAt(rb, r.offset() + i);
    const bool l_false = l_known && !l_true;
    const bool r_false = r_known && !r_true;
    // AND: any FALSE decides FALSE, then both TRUE decides TRUE. OR is the mirror image.
    const bool decided_dominant = is_and ? (l_false || r_false) : (l_true || r_true);
    if (decided_dominant) {
      arrow::bit_util::SetBit(out_valid, i);
      if (!is_and) {
        arrow::bit_util::SetBit(out_bits, i);
      }
    } else if (l_known && r_known) {
      arrow::bit_util::SetBit(out_valid, i);
      if (is_and) {
        arrow::bit_util::SetBit(out_bits, i);
      }
    }
  }
  return FinishArray(arrow::boolean(), n, std::move(validity), std::move(bits));
}

arrow::Result<ArrayPtr> EvalNot(const arrow::Array& input, arrow::MemoryPool* pool) {
  ARROW_RETURN_NOT_OK(CheckBoolean(input, "operand of NOT"));
  const int64_t n = input.length();
  if (n == 0) {
    return arrow::MakeEmptyArray(arrow::boolean());
  }
  ARROW_ASSIGN_OR_RAISE(auto validity, CopyValidity(input, pool));
  ARROW_ASSIGN_OR_RAISE(auto bits, AllocateZeroedBitmap(n, pool));
  uint8_t* out = bits->mutable_data();
  const uint8_t* in = ValueBytes(input);
  const int64_t offset = input.offset();
  ForRows(n, BitsOrNull(validity), [&](int64_t i) {
    if (!BitAt(in, offset + i)) {
      arrow::bit_util::SetBit(out, i);
    }
  });
  if constexpr (kFlawNullAsFalse) {
    // The defect: a NULL operand is treated as FALSE, so NOT NULL is TRUE.
    if (validity != nullptr) {
      for (int64_t i = 0; i < n; ++i) {
        if (!BitAt(validity->data(), i)) {
          arrow::bit_util::SetBit(out, i);
        }
      }
      validity = nullptr;
    }
  }
  return FinishArray(arrow::boolean(), n, std::move(validity), std::move(bits));
}

arrow::Result<ArrayPtr> EvalIsNull(const arrow::Array& input, bool negated,
                                   arrow::MemoryPool* pool) {
  const int64_t n = input.length();
  if (n == 0) {
    return arrow::MakeEmptyArray(arrow::boolean());
  }
  ARROW_ASSIGN_OR_RAISE(auto bits, AllocateZeroedBitmap(n, pool));
  uint8_t* out = bits->mutable_data();
  const uint8_t* valid = input.null_count() > 0 ? input.null_bitmap_data() : nullptr;
  for (int64_t i = 0; i < n; ++i) {
    const bool is_valid = valid == nullptr || BitAt(valid, input.offset() + i);
    if (is_valid == negated) {
      arrow::bit_util::SetBit(out, i);
    }
  }
  return FinishArray(arrow::boolean(), n, nullptr, std::move(bits));
}

}  // namespace quarry::eval_internal
