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

// A batch of `length` rows whose columns are `columns`.
Batch MakeBatch(std::vector<std::shared_ptr<arrow::Array>> columns, int64_t length);

// Charges what a hash table or buffer holds beyond what was charged before.
class ChargedBytes {
 public:
  explicit ChargedBytes(Reservation* reservation) : reservation_(reservation) {}
  // Makes the reservation hold `bytes`: grows it, or shrinks it when `bytes` is less.
  arrow::Status Set(int64_t bytes);

 private:
  Reservation* reservation_;
  int64_t charged_ = 0;
};

// The name an operator charges its memory to: "Sort #3".
std::string ReservationOwner(const char* kind, int id);

}  // namespace quarry

#endif  // QUARRY_EXEC_OPERATOR_UTIL_H_
