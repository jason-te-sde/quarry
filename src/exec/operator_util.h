#ifndef QUARRY_EXEC_OPERATOR_UTIL_H_
#define QUARRY_EXEC_OPERATOR_UTIL_H_

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "exec/exec_context.h"
#include "vector/batch.h"

namespace quarry {

// The bytes the buffers of an array hold.
int64_t ArrayBytes(const arrow::Array& array);
int64_t BatchBytes(const Batch& batch);

// One array from several of the same type, in order; an empty list gives an empty array of `type`.
arrow::Result<std::shared_ptr<arrow::Array>> ConcatenateArrays(
    const std::vector<std::shared_ptr<arrow::Array>>& parts,
    const std::shared_ptr<arrow::DataType>& type, arrow::MemoryPool* pool);

// The most rows, and the most parts, that ConcatenateArrays below joins between two checks for
// cancellation.
constexpr int64_t kConcatenateChunkRows = int64_t{1} << 20;
constexpr size_t kConcatenateChunkParts = size_t{1} << 16;

// Like the overload above, but checks for cancellation and the deadline between pieces: the parts
// are joined in groups of at most kConcatenateChunkRows rows and kConcatenateChunkParts parts, and
// the groups are then joined. A build side made of millions of tiny batches (a batch size of 1) no
// longer runs to the end of one Arrow call before the timeout is looked at. The final join of the
// groups is a single call, a copy of the column.
arrow::Result<std::shared_ptr<arrow::Array>> ConcatenateArrays(
    const ExecContext& context, const std::vector<std::shared_ptr<arrow::Array>>& parts,
    const std::shared_ptr<arrow::DataType>& type);

// A batch of `length` rows whose columns are `columns`.
Batch MakeBatch(std::vector<std::shared_ptr<arrow::Array>> columns, int64_t length);

// Rows [offset, offset + length) of a batch that has no selection; the columns are shared.
Batch SliceBatch(const Batch& batch, int64_t offset, int64_t length);

// How many rows a spilling operator feeds its hash table at a time: all of them without a memory
// limit; with one, few enough that what the table grows by in one step (and the budget does not yet
// know about) stays small next to the budget.
inline int64_t StateSliceRows(const ExecContext& context, int64_t rows) {
  return context.budget->limit() > 0 ? 128 : rows;
}

// The error of a partition that does not fit the budget although it has been split as deep as
// allowed: "resource exhausted: <owner> needs <n> bytes, budget <limit>, in use <used>, ...".
arrow::Status PartitionTooLarge(const ExecContext& context, const std::string& owner,
                                int64_t needed_bytes, int depth);

// Charges what a hash table or buffer holds beyond what was charged before.
class ChargedBytes {
 public:
  explicit ChargedBytes(Reservation* reservation) : reservation_(reservation) {}
  // Makes the reservation hold `bytes`: grows it, or shrinks it when `bytes` is less.
  arrow::Status Set(int64_t bytes);
  // Like Set, but when growing would exceed the budget it changes nothing and returns false.
  bool TrySet(int64_t bytes);
  int64_t charged() const { return charged_; }

 private:
  Reservation* reservation_;
  int64_t charged_ = 0;
};

// Cancelled when the query was cancelled or is past its deadline. The profiling wrapper calls it
// once per Next; loops that run inside one Next (spilling, merging spilled partitions) call it
// themselves, so that a cancelled query ends promptly instead of at the end of the loop.
arrow::Status CheckInterrupted(const ExecContext& context);

// The name an operator charges its memory to: "Sort #3".
std::string ReservationOwner(const char* kind, int id);

}  // namespace quarry

#endif  // QUARRY_EXEC_OPERATOR_UTIL_H_
