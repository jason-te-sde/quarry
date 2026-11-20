#ifndef QUARRY_VECTOR_SELECTION_H_
#define QUARRY_VECTOR_SELECTION_H_

#include <arrow/result.h>

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace quarry {

// The rows of a batch that are still alive, as strictly increasing indices into the batch. The
// only way to build one is Make, which rejects an index that is out of range or not greater than
// the one before it, so every SelectionVector that exists is valid. Debug builds check again each
// time the indices are read.
class SelectionVector {
 public:
  static arrow::Result<SelectionVector> Make(std::vector<uint32_t> indices, int64_t batch_length);

  std::span<const uint32_t> indices() const;
  int64_t size() const { return static_cast<int64_t>(indices_.size()); }
  // The length of the batch these indices point into.
  int64_t batch_length() const { return batch_length_; }

 private:
  SelectionVector(std::vector<uint32_t> indices, int64_t batch_length)
      : indices_(std::move(indices)), batch_length_(batch_length) {}

  bool IsValid() const;

  std::vector<uint32_t> indices_;
  int64_t batch_length_;
};

}  // namespace quarry

#endif  // QUARRY_VECTOR_SELECTION_H_
