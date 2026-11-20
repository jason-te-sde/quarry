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

  // Statistics are indexed by Parquet leaf column. They line up with the Arrow fields only for a
  // flat schema; a nested one gets no statistics rather than misattributed ones.
  const bool flat = metadata->num_columns() == info.schema->num_fields();
  for (int g = 0; g < info.num_row_groups; ++g) {
    std::unique_ptr<parquet::RowGroupMetaData> group = metadata->RowGroup(g);
    RowGroupStats stats;
    stats.num_rows = group->num_rows();
    for (int c = 0; c < info.schema->num_fields(); ++c) {
      if (flat) {
        stats.columns.push_back(ColumnStats(*group->ColumnChunk(c)));
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
