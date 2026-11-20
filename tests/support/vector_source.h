#ifndef QUARRY_TESTS_SUPPORT_VECTOR_SOURCE_H_
#define QUARRY_TESTS_SUPPORT_VECTOR_SOURCE_H_

#include <memory>
#include <vector>

#include "exec/operator.h"
#include "support/arrays.h"

namespace quarry::testing {

// A batch of the given columns (all the same length).
inline Batch BatchOf(std::vector<ArrayPtr> columns) {
  Batch batch;
  batch.length = columns.empty() ? 0 : columns[0]->length();
  batch.columns = std::move(columns);
  return batch;
}

// An operator that hands out prepared batches, and counts how often it was asked for one.
class VectorSource : public Operator {
 public:
  explicit VectorSource(std::vector<Batch> batches, int* next_calls = nullptr)
      : batches_(std::move(batches)), next_calls_(next_calls) {}

  arrow::Status Open(ExecContext&) override { return arrow::Status::OK(); }
  arrow::Result<std::optional<Batch>> Next() override {
    if (next_calls_ != nullptr) ++*next_calls_;
    if (position_ >= batches_.size()) return std::optional<Batch>();
    return std::optional<Batch>(batches_[position_++]);
  }
  void Close() override {}

 private:
  std::vector<Batch> batches_;
  int* next_calls_;
  size_t position_ = 0;
};

inline std::unique_ptr<Operator> Source(std::vector<Batch> batches, int* next_calls = nullptr) {
  return std::make_unique<VectorSource>(std::move(batches), next_calls);
}

}  // namespace quarry::testing

#endif  // QUARRY_TESTS_SUPPORT_VECTOR_SOURCE_H_
