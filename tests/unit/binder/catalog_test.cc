#include "binder/catalog.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "binder/binder.h"
#include "support/parquet_fixture.h"

namespace quarry {
namespace {

using testing::Decimal40Column;
using testing::FloatColumn;
using testing::Int32Column;
using testing::Int64Column;
using testing::ListOfInt32Column;
using testing::StringColumn;
using testing::TempDir;
using testing::TimestampColumn;
using testing::UInt64Column;
using testing::WriteParquet;

const std::filesystem::path kTpch = "tests/data/tpch-sf0.01";

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

TEST(CatalogTest, AddDirectoryRegistersEveryTpchTableLowerCased) {
  Catalog catalog;
  ASSERT_TRUE(catalog.AddDirectory(kTpch).ok());
  for (const char* t :
       {"customer", "lineitem", "nation", "orders", "part", "partsupp", "region", "supplier"}) {
    EXPECT_NE(catalog.Find(t), nullptr) << t;
  }
  EXPECT_EQ(catalog.Find("LineItem"), nullptr) << "lookup is exact; names are stored lower-case";
  const TableInfo* lineitem = catalog.Find("lineitem");
  ASSERT_NE(lineitem, nullptr);
  EXPECT_EQ(lineitem->name, "lineitem");
  EXPECT_EQ(lineitem->columns.size(), 16u);
  EXPECT_EQ(lineitem->columns[0].name, "l_orderkey");
  ASSERT_TRUE(lineitem->columns[0].type.has_value());
  EXPECT_EQ(lineitem->columns[0].type->ToString(), "BIGINT");
  EXPECT_GT(lineitem->row_count, 0);
  EXPECT_EQ(lineitem->files.size(), 1u);
  EXPECT_EQ(lineitem->file_info.size(), 1u);
  EXPECT_EQ(lineitem->row_count, lineitem->file_info[0].num_rows);
}

TEST(CatalogTest, DecimalColumnsAreDecimalsAndDatesAreDates) {
  Catalog catalog;
  ASSERT_TRUE(catalog.AddDirectory(kTpch).ok());
  const TableInfo* lineitem = catalog.Find("lineitem");
  ASSERT_NE(lineitem, nullptr);
  for (const ColumnInfo& c : lineitem->columns) {
    EXPECT_TRUE(c.type.has_value()) << c.name << ": " << c.unsupported_reason;
    if (c.name == "l_quantity") {
      EXPECT_EQ(c.type->ToString(), "DECIMAL(15,2)");
    }
    if (c.name == "l_shipdate") {
      EXPECT_EQ(c.type->ToString(), "DATE");
    }
  }
}

TEST(CatalogTest, AddDirectoryMapsFilesAndSubdirectories) {
  TempDir dir;
  WriteParquet(dir.path() / "Solo.parquet", {Int32Column("a", {1})});
  std::filesystem::create_directories(dir.path() / "multi");
  WriteParquet(dir.path() / "multi" / "part-2.parquet", {Int32Column("a", {3})});
  WriteParquet(dir.path() / "multi" / "part-1.parquet", {Int32Column("a", {1, 2})});
  std::filesystem::create_directories(dir.path() / "empty");
  std::ofstream notes(dir.path() / "notes.txt");
  notes << "not parquet";
  notes.close();

  Catalog catalog;
  ASSERT_TRUE(catalog.AddDirectory(dir.path()).ok());
  const TableInfo* solo = catalog.Find("solo");
  ASSERT_NE(solo, nullptr);
  EXPECT_EQ(solo->files.size(), 1u);
  const TableInfo* multi = catalog.Find("multi");
  ASSERT_NE(multi, nullptr);
  ASSERT_EQ(multi->files.size(), 2u);
  EXPECT_EQ(multi->files[0].filename(), "part-1.parquet");
  EXPECT_EQ(multi->row_count, 3);
  EXPECT_EQ(catalog.Find("empty"), nullptr);
  EXPECT_EQ(catalog.Find("notes"), nullptr);
}

TEST(CatalogTest, RejectsBadRegistrations) {
  TempDir dir;
  const auto file = WriteParquet(dir.path() / "t.parquet", {Int32Column("a", {1})});
  Catalog catalog;
  EXPECT_FALSE(catalog.AddTable("t", {}).ok());
  EXPECT_FALSE(catalog.AddTable("missing", {dir.path() / "nope.parquet"}).ok());
  EXPECT_EQ(catalog.Find("missing"), nullptr);
  ASSERT_TRUE(catalog.AddTable("T", {file}).ok());
  EXPECT_FALSE(catalog.AddTable("t", {file}).ok()) << "duplicate name";
  EXPECT_FALSE(catalog.AddDirectory(dir.path() / "absent").ok());

  const auto clash =
      WriteParquet(dir.path() / "clash.parquet", {Int32Column("A", {1}), Int32Column("a", {2})});
  const arrow::Status st = catalog.AddTable("clash", {clash});
  EXPECT_FALSE(st.ok());
  EXPECT_TRUE(Contains(st.message(), "lower-cased")) << st.message();
}

// Review Focus 1: files that disagree on a column's type.
TEST(CatalogTest, FilesDisagreeingOnATypeRegisterButTheColumnIsUnusable) {
  TempDir dir;
  const auto first =
      WriteParquet(dir.path() / "a.parquet", {Int32Column("x", {1}), Int32Column("y", {1})});
  const auto second =
      WriteParquet(dir.path() / "b.parquet", {Int64Column("x", {2}), Int32Column("y", {2})});
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("t", {first, second}).ok());
  const TableInfo* t = catalog.Find("t");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->row_count, 2);
  EXPECT_FALSE(t->columns[0].type.has_value());
  EXPECT_TRUE(t->columns[1].type.has_value());

  const auto bad = ParseAndBind("select x from t", catalog);
  ASSERT_FALSE(bad.ok());
  const std::string message = bad.status().message();
  EXPECT_EQ(message.rfind("binder error at 1:8: ", 0), 0u) << message;
  EXPECT_TRUE(Contains(message, "x")) << message;
  EXPECT_TRUE(Contains(message, "INTEGER")) << message;
  EXPECT_TRUE(Contains(message, "BIGINT")) << message;
  EXPECT_TRUE(Contains(message, "b.parquet")) << message;

  EXPECT_TRUE(ParseAndBind("select y from t", catalog).ok());
}

struct Unsupported {
  const char* column;
  const char* type_text;
};

TEST(CatalogTest, UnsupportedParquetTypesMakeOnlyTheirColumnUnusable) {
  TempDir dir;
  const auto file = WriteParquet(
      dir.path() / "u.parquet",
      {Int32Column("ok", {1, 2}), TimestampColumn("ts", {0, 1}), UInt64Column("big", {1, 2}),
       FloatColumn("f", {1.0F, 2.0F}), ListOfInt32Column("l", {{1}, {2, 3}}),
       Decimal40Column("d", {100, 200}), StringColumn("s", {"a", "b"})});
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("u", {file}).ok());
  const TableInfo* u = catalog.Find("u");
  ASSERT_NE(u, nullptr);
  ASSERT_EQ(u->columns.size(), 7u);

  EXPECT_TRUE(ParseAndBind("select ok, s from u", catalog).ok());

  const Unsupported cases[] = {
      {"ts", "timestamp"}, {"big", "uint64"}, {"f", "float"}, {"l", "list"}, {"d", "decimal"}};
  for (const Unsupported& c : cases) {
    const auto result = ParseAndBind(std::string("select ") + c.column + " from u", catalog);
    ASSERT_FALSE(result.ok()) << c.column;
    const std::string message = result.status().message();
    EXPECT_EQ(message.rfind("binder error at 1:8: ", 0), 0u) << message;
    EXPECT_TRUE(Contains(message, std::string("column ") + c.column)) << message;
    EXPECT_TRUE(Contains(message, c.type_text)) << message;
    EXPECT_TRUE(Contains(message, "u.parquet")) << message;
  }
}

TEST(CatalogTest, ColumnMissingFromALaterFileIsUnusable) {
  TempDir dir;
  const auto first =
      WriteParquet(dir.path() / "a.parquet", {Int32Column("x", {1}), Int32Column("y", {1})});
  const auto second =
      WriteParquet(dir.path() / "b.parquet", {Int32Column("x", {2}), Int32Column("z", {2})});
  Catalog catalog;
  ASSERT_TRUE(catalog.AddTable("t", {first, second}).ok());
  EXPECT_TRUE(ParseAndBind("select x from t", catalog).ok());
  const auto result = ParseAndBind("select y from t", catalog);
  ASSERT_FALSE(result.ok());
  EXPECT_TRUE(Contains(result.status().message(), "b.parquet")) << result.status().message();
  EXPECT_FALSE(ParseAndBind("select z from t", catalog).ok());
}

}  // namespace
}  // namespace quarry
