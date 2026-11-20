#ifndef QUARRY_EXEC_MEMORY_BUDGET_H_
#define QUARRY_EXEC_MEMORY_BUDGET_H_

#include <arrow/status.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace quarry {

// The bytes of operator state a query may hold at once: hash tables, aggregate states, sort
// buffers, the Top-N buffer and materialized batches. Scan buffers are not charged; they are
// reported in the scan's profile. A limit of 0 means unlimited.
//
// The counters live in a state object that every Reservation shares with the budget, so an operator
// destroyed after the budget (an error path that unwinds in the wrong order) still returns its bytes
// to live memory. The alternative, aborting when a budget dies with reservations outstanding, only
// checks the order in debug builds and turns a lifetime slip into a crash; sharing makes the slip
// impossible in every build for the price of one reference count per reservation.
class MemoryBudget {
 public:
  explicit MemoryBudget(int64_t limit_bytes) : state_(std::make_shared<State>(limit_bytes)) {}
  MemoryBudget(const MemoryBudget&) = delete;
  MemoryBudget& operator=(const MemoryBudget&) = delete;

  int64_t used() const { return state_->used.load(std::memory_order_relaxed); }
  int64_t peak() const { return state_->peak.load(std::memory_order_relaxed); }
  int64_t limit() const { return state_->limit; }

 private:
  friend class Reservation;
  struct State {
    explicit State(int64_t limit_bytes) : limit(limit_bytes) {}
    bool TryAdd(int64_t bytes);
    void Subtract(int64_t bytes) { used.fetch_sub(bytes, std::memory_order_relaxed); }

    const int64_t limit;
    std::atomic<int64_t> used{0};
    std::atomic<int64_t> peak{0};
  };

  std::shared_ptr<State> state_;
};

// The part of a budget one operator holds. Everything it holds is returned to the budget when it
// is destroyed.
class Reservation {
 public:
  Reservation(MemoryBudget& budget, std::string owner)
      : state_(budget.state_), owner_(std::move(owner)) {}
  ~Reservation() { Shrink(size_); }
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;

  // Charges `bytes` more. Over the limit this is OutOfMemory:
  // "resource exhausted: <owner> needs <n> bytes, budget <limit>, in use <used>".
  arrow::Status Grow(int64_t bytes);
  // Charges `bytes` more and returns true, or charges nothing and returns false when that would
  // exceed the limit. Operators that can spill use this.
  bool TryGrow(int64_t bytes);
  // Returns up to `bytes` to the budget.
  void Shrink(int64_t bytes);

  int64_t size() const { return size_; }
  // The most this reservation has held.
  int64_t peak() const { return peak_; }
  const std::string& owner() const { return owner_; }

 private:
  std::shared_ptr<MemoryBudget::State> state_;
  std::string owner_;
  int64_t size_ = 0;
  int64_t peak_ = 0;
};

}  // namespace quarry

#endif  // QUARRY_EXEC_MEMORY_BUDGET_H_
