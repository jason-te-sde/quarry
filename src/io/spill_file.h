#ifndef QUARRY_IO_SPILL_FILE_H_
#define QUARRY_IO_SPILL_FILE_H_

#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "vector/batch.h"

namespace quarry {

// Called with the name of an I/O site ("spill.write", "spill.finish", "spill.read") before the
// operation; an error status makes the operation fail with it. Tests use it to inject faults.
// ExecContext::fault is one. (io may not include exec, so the hook is passed by itself.)
using SpillFault = std::function<arrow::Status(std::string_view site)>;

// The size at which a writer turns its buffered rows into one record batch: small, because a
// partitioned spill keeps one writer per partition open and the buffers are not charged to the
// memory budget.
inline constexpr int64_t kSpillFlushBytes = 4096;

// Writes batches to a file in the Arrow IPC stream format, uncompressed. Rows are buffered until
// kSpillFlushBytes are pending, so that tiny batches (one partition's share of an input batch) do
// not each become a record batch; Finish writes what is left and closes the file.
class SpillWriter {
 public:
  static arrow::Result<std::unique_ptr<SpillWriter>> Open(
      std::filesystem::path path, std::shared_ptr<arrow::Schema> schema, SpillFault fault = {},
      arrow::MemoryPool* pool = arrow::default_memory_pool());
  ~SpillWriter();
  SpillWriter(const SpillWriter&) = delete;
  SpillWriter& operator=(const SpillWriter&) = delete;

  // Appends the live rows of the batch. The batch must have the writer's schema.
  arrow::Status Write(const Batch& batch);
  // Writes the buffered rows and closes the file. Call once; Write is invalid afterwards.
  arrow::Status Finish();

  // Bytes of the file so far (all of them after Finish).
  int64_t bytes_written() const { return bytes_written_; }
  int64_t rows_written() const { return rows_written_; }

 private:
  SpillWriter() = default;
  arrow::Status Flush();

  std::shared_ptr<arrow::Schema> schema_;
  SpillFault fault_;
  arrow::MemoryPool* pool_ = nullptr;
  std::shared_ptr<arrow::io::FileOutputStream> sink_;
  std::shared_ptr<arrow::ipc::RecordBatchWriter> writer_;
  std::vector<std::vector<std::shared_ptr<arrow::Array>>> pending_;  // per column
  int64_t pending_rows_ = 0;
  int64_t pending_bytes_ = 0;
  int64_t bytes_written_ = 0;
  int64_t rows_written_ = 0;
  bool finished_ = false;
};

// Reads a file a SpillWriter wrote, one record batch at a time.
class SpillReader {
 public:
  static arrow::Result<std::unique_ptr<SpillReader>> Open(std::filesystem::path path,
                                                          SpillFault fault = {});
  ~SpillReader() = default;
  SpillReader(const SpillReader&) = delete;
  SpillReader& operator=(const SpillReader&) = delete;

  // The next batch, or std::nullopt at the end of the file.
  arrow::Result<std::optional<Batch>> Next();

 private:
  SpillReader() = default;

  SpillFault fault_;
  std::shared_ptr<arrow::io::ReadableFile> source_;
  std::shared_ptr<arrow::ipc::RecordBatchReader> reader_;
};

}  // namespace quarry

#endif  // QUARRY_IO_SPILL_FILE_H_
