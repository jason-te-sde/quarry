#include <arrow/scalar.h>
#include <arrow/type.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "io/parquet_scan.h"

namespace quarry {
namespace {

// Tests run with the repository root as the working directory.
const std::filesystem::path kEvents = "tests/data/pruning/events.parquet";

TEST(ParquetMetadataTest, ReadsSchemaRowsAndRowGroupStats) {
  auto info = ReadParquetMetadata(kEvents);
  ASSERT_TRUE(info.ok()) << info.status().ToString();

  ASSERT_EQ(info->schema->num_fields(), 3);
  EXPECT_EQ(info->schema->field(0)->name(), "event_date");
  EXPECT_TRUE(info->schema->field(0)->type()->Equals(arrow::date32()));
  EXPECT_EQ(info->schema->field(1)->name(), "id");
  EXPECT_TRUE(info->schema->field(1)->type()->Equals(arrow::int64()));
  EXPECT_EQ(info->schema->field(2)->name(), "amount");
  EXPECT_TRUE(info->schema->field(2)->type()->Equals(arrow::decimal128(12, 2)) ||
              info->schema->field(2)->type()->Equals(arrow::decimal64(12, 2)))
      << info->schema->field(2)->type()->ToString();

  EXPECT_EQ(info->num_rows, 1000);
  EXPECT_EQ(info->num_row_groups, 10);
  ASSERT_EQ(info->row_groups.size(), 10u);

  // One row per day from 2020-01-01 (day 18262 since the epoch): row group 3 holds rows 300..399.
  const auto& group = info->row_groups[3];
  EXPECT_EQ(group.num_rows, 100);
  ASSERT_TRUE(group.columns[0].has_value());
  const auto& dates = *group.columns[0];
  EXPECT_EQ(dates.null_count, 0);
  ASSERT_EQ(dates.min->type->id(), arrow::Type::DATE32);
  EXPECT_EQ(static_cast<const arrow::Date32Scalar&>(*dates.min).value, 18262 + 300);
  EXPECT_EQ(static_cast<const arrow::Date32Scalar&>(*dates.max).value, 18262 + 399);
}

TEST(ParquetMetadataTest, MissingFileIsIOErrorNamingThePath) {
  auto info = ReadParquetMetadata("tests/data/pruning/no_such_file.parquet");
  ASSERT_FALSE(info.ok());
  EXPECT_TRUE(info.status().IsIOError()) << info.status().ToString();
  EXPECT_NE(info.status().message().find("no_such_file.parquet"), std::string::npos)
      << info.status().ToString();
}

}  // namespace
}  // namespace quarry
