#include "support/parquet_fixture.h"

#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>

#include <atomic>
#include <chrono>
#include <system_error>

namespace quarry::testing {
namespace {

template <typename Builder, typename Value>
std::shared_ptr<arrow::Array> Build(Builder* builder, const std::vector<Value>& values) {
  for (const Value& v : values) EXPECT_TRUE(builder->Append(v).ok());
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(builder->Finish(&out).ok());
  return out;
}

Column Make(const std::string& name, std::shared_ptr<arrow::Array> array) {
  return {arrow::field(name, array->type()), std::move(array)};
}

}  // namespace

TempDir::TempDir() {
  static std::atomic<int> counter{0};
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  path_ = std::filesystem::temp_directory_path() /
          ("quarry-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
  std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

Column Int32Column(const std::string& name, const std::vector<int32_t>& values) {
  arrow::Int32Builder b;
  return Make(name, Build(&b, values));
}

Column Int64Column(const std::string& name, const std::vector<int64_t>& values) {
  arrow::Int64Builder b;
  return Make(name, Build(&b, values));
}

Column UInt64Column(const std::string& name, const std::vector<uint64_t>& values) {
  arrow::UInt64Builder b;
  return Make(name, Build(&b, values));
}

Column FloatColumn(const std::string& name, const std::vector<float>& values) {
  arrow::FloatBuilder b;
  return Make(name, Build(&b, values));
}

Column StringColumn(const std::string& name, const std::vector<std::string>& values) {
  arrow::StringBuilder b;
  return Make(name, Build(&b, values));
}

Column TimestampColumn(const std::string& name, const std::vector<int64_t>& micros) {
  arrow::TimestampBuilder b(arrow::timestamp(arrow::TimeUnit::MICRO), arrow::default_memory_pool());
  return Make(name, Build(&b, micros));
}

Column ListOfInt32Column(const std::string& name, const std::vector<std::vector<int32_t>>& values) {
  auto value_builder = std::make_shared<arrow::Int32Builder>();
  arrow::ListBuilder b(arrow::default_memory_pool(), value_builder);
  for (const auto& list : values) {
    EXPECT_TRUE(b.Append().ok());
    for (const int32_t v : list) EXPECT_TRUE(value_builder->Append(v).ok());
  }
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return Make(name, std::move(out));
}

Column StructOfInt64Column(const std::string& name, const std::vector<int64_t>& a,
                           const std::vector<int64_t>& b) {
  arrow::Int64Builder a_builder;
  arrow::Int64Builder b_builder;
  EXPECT_TRUE(a_builder.AppendValues(a).ok());
  EXPECT_TRUE(b_builder.AppendValues(b).ok());
  std::shared_ptr<arrow::Array> a_array;
  std::shared_ptr<arrow::Array> b_array;
  EXPECT_TRUE(a_builder.Finish(&a_array).ok());
  EXPECT_TRUE(b_builder.Finish(&b_array).ok());
  auto out = arrow::StructArray::Make({a_array, b_array}, std::vector<std::string>{"a", "b"});
  EXPECT_TRUE(out.ok()) << out.status().ToString();
  return Make(name, out.ValueOrDie());
}

Column MapOfStringInt64Column(
    const std::string& name,
    const std::vector<std::vector<std::pair<std::string, int64_t>>>& values) {
  auto key_builder = std::make_shared<arrow::StringBuilder>();
  auto item_builder = std::make_shared<arrow::Int64Builder>();
  arrow::MapBuilder b(arrow::default_memory_pool(), key_builder, item_builder);
  for (const auto& entries : values) {
    EXPECT_TRUE(b.Append().ok());
    for (const auto& [key, item] : entries) {
      EXPECT_TRUE(key_builder->Append(key).ok());
      EXPECT_TRUE(item_builder->Append(item).ok());
    }
  }
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return Make(name, std::move(out));
}

Column Decimal40Column(const std::string& name, const std::vector<int64_t>& hundredths) {
  arrow::Decimal256Builder b(arrow::decimal256(40, 2));
  for (const int64_t v : hundredths) EXPECT_TRUE(b.Append(arrow::Decimal256(v)).ok());
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return Make(name, std::move(out));
}

std::filesystem::path WriteParquetRowGroups(const std::filesystem::path& file,
                                            const std::vector<Column>& columns,
                                            int64_t row_group_size) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  for (const Column& c : columns) {
    fields.push_back(c.first);
    arrays.push_back(c.second);
  }
  auto table = arrow::Table::Make(arrow::schema(fields), arrays);
  auto out = arrow::io::FileOutputStream::Open(file.string());
  EXPECT_TRUE(out.ok()) << out.status().ToString();
  if (out.ok()) {
    const arrow::Status st =
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, row_group_size);
    EXPECT_TRUE(st.ok()) << st.ToString();
    EXPECT_TRUE((*out)->Close().ok());
  }
  return file;
}

std::filesystem::path WriteParquet(const std::filesystem::path& file,
                                   const std::vector<Column>& columns) {
  return WriteParquetRowGroups(file, columns, 1024);
}

std::filesystem::path WriteParquetWithoutRowGroups(const std::filesystem::path& file,
                                                   const std::vector<Column>& columns) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  for (const Column& c : columns) fields.push_back(c.first);
  auto out = arrow::io::FileOutputStream::Open(file.string());
  EXPECT_TRUE(out.ok()) << out.status().ToString();
  if (!out.ok()) return file;
  auto writer =
      parquet::arrow::FileWriter::Open(*arrow::schema(fields), arrow::default_memory_pool(), *out);
  EXPECT_TRUE(writer.ok()) << writer.status().ToString();
  if (writer.ok()) {
    EXPECT_TRUE((*writer)->Close().ok());
  }
  EXPECT_TRUE((*out)->Close().ok());
  return file;
}

}  // namespace quarry::testing
