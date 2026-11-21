#ifndef QUARRY_EXEC_EXEC_CONTEXT_H_
#define QUARRY_EXEC_EXEC_CONTEXT_H_

#include <arrow/memory_pool.h>
#include <arrow/status.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string_view>

#include "exec/memory_budget.h"

namespace quarry {

class SpillDirectory;  // io/spill_directory.h

// What every operator of one query shares. The pointers are owned by the caller of the query and
// outlive the operators.
struct ExecContext {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  // Where scans allocate (the Parquet reader's buffers and the batches cut from them); null means
  // `pool`. The engine gives scans a pool of their own so that the memory the budget does not
  // charge can be measured with and without them.
  arrow::MemoryPool* scan_pool = nullptr;
  MemoryBudget* budget = nullptr;
  SpillDirectory* spill = nullptr;
  int32_t batch_size = 2048;
  // Set from another thread to end the query; checked once per Next call.
  const std::atomic<bool>* cancelled = nullptr;
  // A query still running at this time ends as Cancelled. time_point::max() means no timeout.
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  // The timeout that set the deadline, for the error message; zero when there is none.
  std::chrono::milliseconds timeout{0};
  // Null outside tests. Spill I/O calls it with the name of the site and fails the operation when
  // it returns an error, so that tests can inject I/O faults.
  std::function<arrow::Status(std::string_view site)> fault;
};

}  // namespace quarry

#endif  // QUARRY_EXEC_EXEC_CONTEXT_H_
