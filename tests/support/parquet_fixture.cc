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

Column Decimal40Column(const std::string& name, const std::vector<int64_t>& hundredths) {
  arrow::Decimal256Builder b(arrow::decimal256(40, 2));
  for (const int64_t v : hundredths) EXPECT_TRUE(b.Append(arrow::Decimal256(v)).ok());
  std::shared_ptr<arrow::Array> out;
  EXPECT_TRUE(b.Finish(&out).ok());
  return Make(name, std::move(out));
}

std::filesystem::path WriteParquet(const std::filesystem::path& file,
                                   const std::vector<Column>& columns) {
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
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, 1024);
    EXPECT_TRUE(st.ok()) << st.ToString();
    EXPECT_TRUE((*out)->Close().ok());
  }
  return file;
}

}  // namespace quarry::testing
