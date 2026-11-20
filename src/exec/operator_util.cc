#include "exec/operator_util.h"

#include <arrow/array/concatenate.h>
#include <arrow/array/util.h>
#include <arrow/util/byte_size.h>

namespace quarry {

int64_t ArrayBytes(const arrow::Array& array) {
  // The bytes the array references, offset and length respected: a batch that is a slice of a
  // whole row group is charged for its own rows, not for the row group. (A retained slice does keep
  // the row group alive; that memory is the scan's buffer, reported as scan_buffer_bytes.)
  auto referenced = arrow::util::ReferencedBufferSize(array);
  if (referenced.ok()) return *referenced;
  int64_t bytes = 0;
  for (const auto& buffer : array.data()->buffers) {
    if (buffer != nullptr) bytes += buffer->size();
  }
  return bytes;
}

int64_t BatchBytes(const Batch& batch) {
  int64_t bytes = 0;
  for (const auto& column : batch.columns) bytes += ArrayBytes(*column);
  return bytes;
}

arrow::Result<std::shared_ptr<arrow::Array>> ConcatenateArrays(
    const std::vector<std::shared_ptr<arrow::Array>>& parts,
    const std::shared_ptr<arrow::DataType>& type, arrow::MemoryPool* pool) {
  if (parts.empty()) return arrow::MakeEmptyArray(type, pool);
  if (parts.size() == 1) return parts[0];
  return arrow::Concatenate(parts, pool);
}

Batch MakeBatch(std::vector<std::shared_ptr<arrow::Array>> columns, int64_t length) {
  Batch batch;
  batch.columns = std::move(columns);
  batch.length = length;
  return batch;
}

arrow::Status ChargedBytes::Set(int64_t bytes) {
  if (bytes > charged_) {
    ARROW_RETURN_NOT_OK(reservation_->Grow(bytes - charged_));
  } else if (bytes < charged_) {
    reservation_->Shrink(charged_ - bytes);
  }
  charged_ = bytes;
  return arrow::Status::OK();
}

std::string ReservationOwner(const char* kind, int id) {
  return std::string(kind) + " #" + std::to_string(id);
}

}  // namespace quarry
