#include "vector/batch.h"

#include <arrow/status.h>

#include "vector/gather.h"

namespace quarry {

arrow::Result<Batch> Batch::Compact(arrow::MemoryPool* pool) const {
  if (!selection) {
    return *this;
  }
  Batch out;
  out.length = selection->size();
  out.columns.reserve(columns.size());
  for (const auto& column : columns) {
    if (column->length() != length) {
      return arrow::Status::Invalid("batch column has ", column->length(), " rows, the batch has ",
                                    length);
    }
    ARROW_ASSIGN_OR_RAISE(auto gathered, Gather(*column, selection->indices(), pool));
    out.columns.push_back(std::move(gathered));
  }
  return out;
}

}  // namespace quarry
