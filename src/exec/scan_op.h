#ifndef QUARRY_EXEC_SCAN_OP_H_
#define QUARRY_EXEC_SCAN_OP_H_

#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <memory>
#include <utility>
#include <vector>

#include "exec/operator.h"
#include "plan/physical.h"

namespace quarry {

// What a scan did, for tests and EXPLAIN ANALYZE.
struct ScanReport {
  // (file index, row group index) of every row group that was read, in reading order.
  std::vector<std::pair<int, int>> row_groups_read;
  int row_groups_total = 0;
  // Columns decoded per row group read: the columns the scan outputs, or the first column of the
  // file when it outputs none.
  int columns_decoded = 0;
};

// Reads the Parquet files of a PhysicalScan one row group at a time, skipping the row groups that
// the prune conjuncts refute, decoding only the columns the scan outputs, converting DECIMAL
// columns of precision <= 18 from the reader's decimal128 to decimal64, and applying the scan's
// predicate. Columns are matched to the files by (lower-cased) name. Batches are slices of a row
// group, of at most batch_size rows.
class ScanOperator : public Operator {
 public:
  explicit ScanOperator(const PhysicalScan& node);
  ~ScanOperator() override;

  arrow::Status Open(ExecContext& context) override;
  arrow::Result<std::optional<Batch>> Next() override;
  void Close() override;
  void FillStats(OperatorStats* stats) const override;

  const ScanReport& report() const { return report_; }

 private:
  struct FileState;

  arrow::Status OpenFile(size_t index);
  // Loads the next row group that survives pruning; false at the end of the last file.
  arrow::Result<bool> LoadNextRowGroup();

  const PhysicalScan& node_;
  ExecContext* context_ = nullptr;
  std::vector<std::unique_ptr<FileState>> files_;
  size_t file_ = 0;
  int row_group_ = -1;
  std::vector<std::shared_ptr<arrow::Array>> columns_;  // the current row group, one array each
  int64_t group_rows_ = 0;
  int64_t position_ = 0;
  int64_t scan_buffer_bytes_ = 0;
  ScanReport report_;
};

}  // namespace quarry

#endif  // QUARRY_EXEC_SCAN_OP_H_
