#ifndef QUARRY_ENGINE_H_
#define QUARRY_ENGINE_H_

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace quarry {

struct EngineOptions {
  int64_t memory_limit_bytes = 0;  // 0 = unlimited
  int32_t batch_size = 2048;
  std::set<std::string> disabled_rules;  // fold_constants, push_filters, prune_row_groups, ...
  std::filesystem::path spill_parent;    // empty = std::filesystem::temp_directory_path()
  std::chrono::milliseconds timeout{0};  // 0 = none
  // For tests: called with "spill.write", "spill.finish" or "spill.read" before each spill I/O
  // operation; an error status fails that operation with it.
  std::function<arrow::Status(std::string_view site)> spill_fault;
};

// What one operator of a query did.
struct OperatorProfile {
  int id = 0;  // the operator's number in the plan, as EXPLAIN prints it
  std::string name;
  int64_t rows_out = 0;
  int64_t batches_out = 0;
  int64_t exclusive_ns = 0;  // own time: without the time spent in its inputs
  int64_t peak_state_bytes = 0;
  int64_t spilled_bytes = 0;
  int64_t scan_buffer_bytes = 0;
};

struct QueryResult {
  std::shared_ptr<arrow::Table> table;
  std::vector<OperatorProfile> profile;  // one entry per operator, by id
  // The most memory the query's operators held through Arrow's memory pool at once: the hash
  // tables' storage (key index, key arena, aggregate states, join chains), build rows, sort
  // buffers, spill buffers and concatenation copies, whether or not the memory budget charged them.
  // The strings that MIN and MAX of a VARCHAR keep are not in it. The result table is copied out of
  // the pool as it is produced, so it is not counted. Scans allocate from a pool of their own,
  // reported next to it.
  //
  // With a memory limit, this stays within the limit plus 256 KiB for what the budget charges. A
  // join's concatenation of its build rows is not charged: it makes a second copy, and that copy is
  // in this pool when the build input is filtered, is another operator's output or is read back from
  // a spill file, so the peak can pass the limit by up to the in-memory build payload (more above
  // 2^20 build rows). See docs/design/0006-memory-and-spill.md and the known issues in CHANGELOG.md.
  int64_t pool_peak_bytes = 0;
  int64_t scan_pool_peak_bytes = 0;
};

// A SQL query engine over Parquet tables. Register tables, then Execute queries. Every error is a
// Status: "syntax error at L:C: ...", "binder error at L:C: ...", "planner error at L:C: ...",
// "resource exhausted: ..." (OutOfMemory) and Cancelled for Cancel() and timeouts.
//
// An Engine runs one query at a time. Only Cancel may be called while a query runs, and from any
// thread; a second Execute or Explain from another thread returns Invalid ("engine is busy").
class Engine {
 public:
  explicit Engine(EngineOptions options);
  ~Engine();  // NOLINT(performance-trivially-destructible): Impl is incomplete here
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Registers a table made of one or more Parquet files that share their schema by column name.
  arrow::Status RegisterTable(std::string name, std::vector<std::filesystem::path> files);
  // Registers every `x.parquet` of the directory as table `x`, and every subdirectory of Parquet
  // files `x/` as table `x`.
  arrow::Status RegisterDirectory(const std::filesystem::path& dir);

  arrow::Result<QueryResult> Execute(std::string_view sql);

  // The physical plan, one operator per line with its row estimate. With `analyze` the query runs
  // and every line also shows what the operator did: rows=, batches=, time=, peak=.
  arrow::Result<std::string> Explain(std::string_view sql, bool analyze);

  // Ends the running query with Cancelled at the next batch boundary. Safe from any thread (and
  // from a signal handler); a no-op when nothing runs. A query that starts afterwards is not
  // affected.
  void Cancel();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace quarry

#endif  // QUARRY_ENGINE_H_
