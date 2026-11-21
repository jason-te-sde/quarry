#include <arrow/status.h>
#include <arrow/util/bitmap_ops.h>

#include "expr/eval_internal.h"

namespace quarry::eval_internal {

arrow::Result<BufferPtr> AllocateZeroedBitmap(int64_t n, arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(auto bitmap, arrow::AllocateEmptyBitmap(n, pool));
  return BufferPtr(std::move(bitmap));
}

arrow::Result<BufferPtr> CopyValidity(const arrow::Array& a, arrow::MemoryPool* pool) {
  if (a.null_count() == 0) {
    return BufferPtr();
  }
  ARROW_ASSIGN_OR_RAISE(
      auto copy, arrow::internal::CopyBitmap(pool, a.null_bitmap_data(), a.offset(), a.length()));
  return BufferPtr(std::move(copy));
}

arrow::Result<BufferPtr> AndValidity(const arrow::Array& a, const arrow::Array& b,
                                     arrow::MemoryPool* pool) {
  if (a.null_count() == 0) {
    return CopyValidity(b, pool);
  }
  if (b.null_count() == 0) {
    return CopyValidity(a, pool);
  }
  ARROW_ASSIGN_OR_RAISE(
      auto both, arrow::internal::BitmapAnd(pool, a.null_bitmap_data(), a.offset(),
                                            b.null_bitmap_data(), b.offset(), a.length(), 0));
  return BufferPtr(std::move(both));
}

ArrayPtr FinishArray(const std::shared_ptr<arrow::DataType>& type, int64_t n, BufferPtr validity,
                     BufferPtr values) {
  int64_t null_count = 0;
  if (validity != nullptr) {
    null_count = n - arrow::internal::CountSetBits(validity->data(), 0, n);
    if (null_count == 0) {
      validity = nullptr;
    }
  }
  return arrow::MakeArray(
      arrow::ArrayData::Make(type, n, {std::move(validity), std::move(values)}, null_count));
}

arrow::Status CheckArrayType(const arrow::Array& array, const LogicalType& type, const char* what) {
  if (!array.type()->Equals(*type.ToArrow())) {
    return arrow::Status::Invalid(what, ": expected ", type.ToArrow()->ToString(), " but found ",
                                  array.type()->ToString());
  }
  return arrow::Status::OK();
}

}  // namespace quarry::eval_internal
