#include "exec/memory_budget.h"

#include <algorithm>

namespace quarry {

bool MemoryBudget::TryAdd(int64_t bytes) {
  int64_t current = used_.load(std::memory_order_relaxed);
  for (;;) {
    if (limit_ > 0 && current + bytes > limit_) {
      return false;
    }
    if (used_.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed)) {
      break;
    }
  }
  int64_t seen = peak_.load(std::memory_order_relaxed);
  while (seen < current + bytes &&
         !peak_.compare_exchange_weak(seen, current + bytes, std::memory_order_relaxed)) {
  }
  return true;
}

bool Reservation::TryGrow(int64_t bytes) {
  if (bytes < 0) {
    return false;
  }
  if (!budget_->TryAdd(bytes)) {
    return false;
  }
  size_ += bytes;
  peak_ = std::max(peak_, size_);
  return true;
}

arrow::Status Reservation::Grow(int64_t bytes) {
  if (TryGrow(bytes)) {
    return arrow::Status::OK();
  }
  return arrow::Status::OutOfMemory("resource exhausted: ", owner_, " needs ", bytes,
                                    " bytes, budget ", budget_->limit(), ", in use ",
                                    budget_->used());
}

void Reservation::Shrink(int64_t bytes) {
  const int64_t returned = std::clamp<int64_t>(bytes, 0, size_);
  size_ -= returned;
  budget_->Subtract(returned);
}

}  // namespace quarry
