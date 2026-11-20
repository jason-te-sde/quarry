#ifndef QUARRY_BINDER_CATALOG_H_
#define QUARRY_BINDER_CATALOG_H_

#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "io/parquet_scan.h"
#include "types/logical_type.h"

namespace quarry {

// One column of a table. A column the engine cannot use (an unsupported Parquet type, or a type
// that differs between the files of the table) still has an entry, with no type and the reason, so
// that the table registers and the other columns stay usable; naming such a column in a query is a
// binder error that quotes the reason.
struct ColumnInfo {
  std::string name;  // lower-cased
  std::optional<LogicalType> type;
  std::string unsupported_reason;  // empty when `type` is set
};

struct TableInfo {
  std::string name;  // lower-cased
  std::vector<std::filesystem::path> files;
  std::vector<ColumnInfo> columns;         // the columns of the first file, in file order
  int64_t row_count = 0;                   // over all files
  std::vector<ParquetFileInfo> file_info;  // parallel to `files`
};

// The tables a query can name. Table and column names are stored lower-cased, so unquoted SQL
// identifiers (lower-cased by the parser) find them and quoted ones must be written in lower case.
class Catalog {
 public:
  // Registers a table over one or more Parquet files that share a schema by column name. The
  // footers are read; no data is. Invalid when there is no file, the name is taken, or two columns
  // of the first file have the same name once lower-cased; an unreadable file is the IOError that
  // ReadParquetMetadata reports. A column that has an unsupported type, or a different type in
  // another file, or is missing from another file, does not fail the registration: it is unusable
  // and says why. Columns that only a later file has are ignored.
  arrow::Status AddTable(std::string name, std::vector<std::filesystem::path> files);

  // Registers every `x.parquet` of the directory as table `x`, and every subdirectory `x/` that
  // holds `.parquet` files as table `x` made of those files (in name order). Other entries are
  // ignored.
  arrow::Status AddDirectory(const std::filesystem::path& directory);

  // The table of that exact (lower-case) name, or null. The pointer stays valid as long as the
  // Catalog lives, including across later AddTable calls.
  const TableInfo* Find(std::string_view name) const;

 private:
  std::map<std::string, TableInfo, std::less<>> tables_;
};

}  // namespace quarry

#endif  // QUARRY_BINDER_CATALOG_H_
