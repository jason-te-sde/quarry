#include "io/parquet_scan.h"

#include <arrow/io/file.h>
#include <arrow/status.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/schema.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>

#include <string>
#include <utility>

namespace quarry {
namespace {

std::optional<ColumnMinMax> ColumnStats(const parquet::ColumnChunkMetaData& chunk) {
  if (!chunk.is_stats_set()) {
    return std::nullopt;
  }
  std::shared_ptr<parquet::Statistics> stats = chunk.statistics();
  if (stats == nullptr || !stats->HasMinMax()) {
    return std::nullopt;
  }
  ColumnMinMax out;
  if (!parquet::arrow::StatisticsAsScalars(*stats, &out.min, &out.max).ok() || out.min == nullptr ||
      out.max == nullptr) {
    return std::nullopt;
  }
  out.null_count = stats->HasNullCount() ? stats->null_count() : 0;
  return out;
}

arrow::Result<ParquetFileInfo> ReadFooter(const std::filesystem::path& path) {
  ARROW_ASSIGN_OR_RAISE(auto file, arrow::io::ReadableFile::Open(path.string()));
  std::unique_ptr<parquet::ParquetFileReader> reader = parquet::ParquetFileReader::Open(file);
  std::shared_ptr<parquet::FileMetaData> metadata = reader->metadata();

  ParquetFileInfo info;
  ARROW_RETURN_NOT_OK(parquet::arrow::FromParquetSchema(
      metadata->schema(), parquet::default_arrow_reader_properties(),
      metadata->key_value_metadata(), &info.schema));
  info.num_rows = metadata->num_rows();
  info.num_row_groups = metadata->num_row_groups();

  parquet::arrow::SchemaManifest manifest;
  ARROW_RETURN_NOT_OK(
      parquet::arrow::SchemaManifest::Make(metadata->schema(), metadata->key_value_metadata(),
                                           parquet::default_arrow_reader_properties(), &manifest));
  if (static_cast<int>(manifest.schema_fields.size()) != info.schema->num_fields()) {
    return arrow::Status::Invalid(path.string(),
                                  ": the Parquet schema and its Arrow form disagree");
  }
  info.num_leaves = metadata->num_columns();
  for (const parquet::arrow::SchemaField& field : manifest.schema_fields) {
    info.field_leaf.push_back(field.is_leaf() ? field.column_index : -1);
  }

  // Statistics are indexed by Parquet leaf column, not by Arrow field: a nested field has no
  // statistics of its own, and the fields after it sit at different positions in the two.
  for (int g = 0; g < info.num_row_groups; ++g) {
    std::unique_ptr<parquet::RowGroupMetaData> group = metadata->RowGroup(g);
    RowGroupStats stats;
    stats.num_rows = group->num_rows();
    for (const int leaf : info.field_leaf) {
      if (leaf >= 0) {
        stats.columns.push_back(ColumnStats(*group->ColumnChunk(leaf)));
      } else {
        stats.columns.emplace_back(std::nullopt);
      }
    }
    info.row_groups.push_back(std::move(stats));
  }
  return info;
}

}  // namespace

arrow::Result<ParquetFileInfo> ReadParquetMetadata(const std::filesystem::path& path) {
  // The Parquet reader reports a malformed footer by throwing; nothing escapes this function.
  try {
    return ReadFooter(path);
  } catch (const parquet::ParquetStatusException& e) {
    return e.status().WithMessage(path.string() + ": " + e.status().message());
  } catch (const parquet::ParquetException& e) {
    return arrow::Status::IOError(path.string(), ": ", e.what());
  }
}

}  // namespace quarry
