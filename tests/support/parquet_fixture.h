#ifndef QUARRY_TESTS_SUPPORT_PARQUET_FIXTURE_H_
#define QUARRY_TESTS_SUPPORT_PARQUET_FIXTURE_H_

#include <arrow/api.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace quarry::testing {

// A scratch directory under the system temp directory, removed with the object.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

using Column = std::pair<std::shared_ptr<arrow::Field>, std::shared_ptr<arrow::Array>>;

Column Int32Column(const std::string& name, const std::vector<int32_t>& values);
Column Int64Column(const std::string& name, const std::vector<int64_t>& values);
Column UInt64Column(const std::string& name, const std::vector<uint64_t>& values);
Column FloatColumn(const std::string& name, const std::vector<float>& values);
Column StringColumn(const std::string& name, const std::vector<std::string>& values);
Column TimestampColumn(const std::string& name, const std::vector<int64_t>& micros);
Column ListOfInt32Column(const std::string& name, const std::vector<std::vector<int32_t>>& values);
// STRUCT<a BIGINT, b BIGINT>: two Parquet leaf columns under one Arrow field.
Column StructOfInt64Column(const std::string& name, const std::vector<int64_t>& a,
                           const std::vector<int64_t>& b);
// MAP<VARCHAR, BIGINT>: two Parquet leaf columns (key and value) under one Arrow field.
Column MapOfStringInt64Column(
    const std::string& name,
    const std::vector<std::vector<std::pair<std::string, int64_t>>>& values);
// DECIMAL(40, 2): wider than the engine's 38 digits.
Column Decimal40Column(const std::string& name, const std::vector<int64_t>& hundredths);

// Writes the columns as one Parquet file at `file` (its directory must exist) and returns `file`.
// Records a test failure and returns it anyway if writing fails.
std::filesystem::path WriteParquet(const std::filesystem::path& file,
                                   const std::vector<Column>& columns);

// Writes a valid Parquet file that has the schema of `columns` and no row group at all.
std::filesystem::path WriteParquetWithoutRowGroups(const std::filesystem::path& file,
                                                   const std::vector<Column>& columns);

// Like WriteParquet, with row groups of `row_group_size` rows.
std::filesystem::path WriteParquetRowGroups(const std::filesystem::path& file,
                                            const std::vector<Column>& columns,
                                            int64_t row_group_size);

}  // namespace quarry::testing

#endif  // QUARRY_TESTS_SUPPORT_PARQUET_FIXTURE_H_
