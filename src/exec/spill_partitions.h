#ifndef QUARRY_EXEC_SPILL_PARTITIONS_H_
#define QUARRY_EXEC_SPILL_PARTITIONS_H_

// What the spilling operators share: how rows are split by hash into partitions, and the set of
// files one split writes. A row's partition at depth d is four bits of its key hash, the top ones
// for depth 0 and the next four for each deeper level, so that rows that collided into one
// partition spread out at the next. The top bits are used because the hash tables index their
// slots with the low ones.

#include <arrow/array.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "exec/exec_context.h"
#include "io/spill_file.h"
#include "vector/batch.h"

namespace quarry {

inline constexpr int kSpillFanout = 16;
// A partition that still does not fit after being split this many times (so at depth 3, the fourth
// split) fails with "resource exhausted": its rows share too much of their key hash to be
// separated.
inline constexpr int kMaxSpillDepth = 3;

inline int PartitionOf(uint64_t hash, int depth) {
  return static_cast<int>((hash >> (60 - 4 * depth)) & 0xF);
}

// A schema with one nullable field per type, named c0, c1, ...
std::shared_ptr<arrow::Schema> SpillSchema(
    const std::vector<std::shared_ptr<arrow::DataType>>& types);

// The hash of each of the `n` rows of the key columns (the same bytes the hash tables compare), and
// whether the row has a NULL key.
arrow::Status HashKeyRows(const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t n,
                          std::vector<uint64_t>* hashes, std::vector<uint8_t>* any_null);

// Up to kSpillFanout files, one per partition, opened when the partition gets its first row.
class SpillPartitions {
 public:
  SpillPartitions(ExecContext& context, std::shared_ptr<arrow::Schema> schema, std::string prefix,
                  int depth);

  // Writes row i of `batch` (which must have no selection) to its partition, except the rows with
  // skip[i] != 0 (`skip` may be null). Checks for cancellation first.
  arrow::Status Append(const Batch& batch, std::span<const uint64_t> hashes, const uint8_t* skip);
  // Finishes every file. Call once, after the last Append.
  arrow::Status Finish();

  int depth() const { return depth_; }
  int64_t rows(int partition) const { return rows_[static_cast<size_t>(partition)]; }
  const std::filesystem::path& path(int partition) const {
    return paths_[static_cast<size_t>(partition)];
  }
  // Bytes written to the files so far (all of them after Finish).
  int64_t bytes_written() const;

 private:
  ExecContext& context_;
  std::shared_ptr<arrow::Schema> schema_;
  std::string prefix_;
  int depth_;
  std::array<std::unique_ptr<SpillWriter>, kSpillFanout> writers_;
  std::array<std::filesystem::path, kSpillFanout> paths_;
  std::array<int64_t, kSpillFanout> rows_{};
  int64_t finished_bytes_ = 0;
};

// Deletes a spill file once it has been read; the directory removes whatever is left.
void RemoveSpillFile(const std::filesystem::path& path);

}  // namespace quarry

#endif  // QUARRY_EXEC_SPILL_PARTITIONS_H_
