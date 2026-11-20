#ifndef QUARRY_IO_PARQUET_SCAN_H_
#define QUARRY_IO_PARQUET_SCAN_H_

#include <arrow/result.h>
#include <arrow/scalar.h>
#include <arrow/type.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace quarry {

// Minimum, maximum and null count of one column in one row group, as Arrow scalars of the column's
// own type.
struct ColumnMinMax {
  std::shared_ptr<arrow::Scalar> min;
  std::shared_ptr<arrow::Scalar> max;
  int64_t null_count = 0;
};

// Statistics of one row group. A column has no entry (std::nullopt) when the file recorded no
// usable minimum and maximum for it, or when its type has no ordering that Arrow scalars express.
struct RowGroupStats {
  int64_t num_rows = 0;
  std::vector<std::optional<ColumnMinMax>> columns;
};

struct ParquetFileInfo {
  std::shared_ptr<arrow::Schema> schema;
  int64_t num_rows = 0;
  int num_row_groups = 0;
  std::vector<RowGroupStats> row_groups;
};

// Reads the footer of a Parquet file: schema, row counts and per-row-group column statistics. No
// column data is read. A file that cannot be opened is an arrow::Status::IOError that names the
// path.
arrow::Result<ParquetFileInfo> ReadParquetMetadata(const std::filesystem::path& path);

}  // namespace quarry

#endif  // QUARRY_IO_PARQUET_SCAN_H_
