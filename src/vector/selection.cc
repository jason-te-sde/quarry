#include "vector/selection.h"

#include <arrow/status.h>

#include <cassert>
#include <utility>

namespace quarry {

arrow::Result<SelectionVector> SelectionVector::Make(std::vector<uint32_t> indices,
                                                     int64_t batch_length) {
  if (batch_length < 0) {
    return arrow::Status::Invalid("selection vector: negative batch length ", batch_length);
  }
  for (size_t i = 0; i < indices.size(); ++i) {
    if (static_cast<int64_t>(indices[i]) >= batch_length) {
      return arrow::Status::Invalid("selection vector: index ", indices[i], " at position ", i,
                                    " is out of range for a batch of ", batch_length, " rows");
    }
    if (i > 0 && indices[i] <= indices[i - 1]) {
      return arrow::Status::Invalid("selection vector: index ", indices[i], " at position ", i,
                                    " does not increase (previous ", indices[i - 1], ")");
    }
  }
  return SelectionVector(std::move(indices), batch_length);
}

bool SelectionVector::IsValid() const {
  for (size_t i = 0; i < indices_.size(); ++i) {
    if (static_cast<int64_t>(indices_[i]) >= batch_length_ ||
        (i > 0 && indices_[i] <= indices_[i - 1])) {
      return false;
    }
  }
  return true;
}

std::span<const uint32_t> SelectionVector::indices() const {
  assert(IsValid());
  return indices_;
}

}  // namespace quarry
