#ifndef QUARRY_VECTOR_BATCH_H_
#define QUARRY_VECTOR_BATCH_H_

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "vector/selection.h"

namespace quarry {

// A set of equally long columns plus, optionally, the rows of them that are alive. A filter only
// sets the selection; the operators that need contiguous data call Compact.
struct Batch {
  std::vector<std::shared_ptr<arrow::Array>> columns;
  std::optional<SelectionVector> selection;
  int64_t length = 0;

  // The number of live rows: the selection's size, or length when there is none.
  int64_t active_rows() const { return selection ? selection->size() : length; }

  // The live rows only, in selection order, without a selection. A batch without a selection is
  // returned as is (columns shared, nothing copied).
  arrow::Result<Batch> Compact(arrow::MemoryPool* pool) const;
};

}  // namespace quarry

#endif  // QUARRY_VECTOR_BATCH_H_
