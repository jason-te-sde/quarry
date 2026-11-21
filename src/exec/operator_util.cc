#include "exec/operator_util.h"

#include <arrow/array/concatenate.h>
#include <arrow/array/util.h>
#include <arrow/util/byte_size.h>

#include <chrono>

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

arrow::Result<std::shared_ptr<arrow::Array>> ConcatenateArrays(
    const ExecContext& context, const std::vector<std::shared_ptr<arrow::Array>>& parts,
    const std::shared_ptr<arrow::DataType>& type) {
  std::vector<std::shared_ptr<arrow::Array>> pieces;
  std::vector<std::shared_ptr<arrow::Array>> group;
  int64_t group_rows = 0;
  const auto flush = [&]() -> arrow::Status {
    ARROW_RETURN_NOT_OK(CheckInterrupted(context));
    ARROW_ASSIGN_OR_RAISE(auto piece, ConcatenateArrays(group, type, context.pool));
    pieces.push_back(std::move(piece));
    group.clear();
    group_rows = 0;
    return arrow::Status::OK();
  };
  for (const auto& part : parts) {
    group.push_back(part);
    group_rows += part->length();
    if (group_rows >= kConcatenateChunkRows || group.size() >= kConcatenateChunkParts) {
      ARROW_RETURN_NOT_OK(flush());
    }
  }
  if (!group.empty() || pieces.empty()) ARROW_RETURN_NOT_OK(flush());
  ARROW_RETURN_NOT_OK(CheckInterrupted(context));
  return ConcatenateArrays(pieces, type, context.pool);
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

Batch SliceBatch(const Batch& batch, int64_t offset, int64_t length) {
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(batch.columns.size());
  for (const auto& column : batch.columns) columns.push_back(column->Slice(offset, length));
  return MakeBatch(std::move(columns), length);
}

arrow::Status PartitionTooLarge(const ExecContext& context, const std::string& owner,
                                int64_t needed_bytes, int depth) {
  return arrow::Status::OutOfMemory(
      "resource exhausted: ", owner, " needs ", needed_bytes, " bytes, budget ",
      context.budget->limit(), ", in use ", context.budget->used(),
      ": a partition does not fit although the input was split by hash down to partition depth ",
      depth,
      "; its rows share too much of their key hash to be separated (a heavily repeated "
      "key?)");
}

bool ChargedBytes::TrySet(int64_t bytes) {
  if (bytes > charged_) {
    if (!reservation_->TryGrow(bytes - charged_)) return false;
  } else if (bytes < charged_) {
    reservation_->Shrink(charged_ - bytes);
  }
  charged_ = bytes;
  return true;
}

arrow::Status CheckInterrupted(const ExecContext& context) {
  if (context.cancelled != nullptr && context.cancelled->load(std::memory_order_relaxed)) {
    return arrow::Status::Cancelled("query cancelled");
  }
  if (context.deadline != std::chrono::steady_clock::time_point::max() &&
      std::chrono::steady_clock::now() >= context.deadline) {
    return arrow::Status::Cancelled("query cancelled: the timeout of ", context.timeout.count(),
                                    " ms was exceeded");
  }
  return arrow::Status::OK();
}

std::string ReservationOwner(const char* kind, int id) {
  return std::string(kind) + " #" + std::to_string(id);
}

}  // namespace quarry
