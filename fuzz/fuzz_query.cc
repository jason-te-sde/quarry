// libFuzzer target: bytes become a query, through a small grammar over two fixed tables, and run on
// an engine whose options the bytes also choose (batch size, memory limit, one disabled rule). The
// grammar produces mostly valid queries, so that the input reaches the binder, the planner and the
// operators, not only the parser; some are still rejected, and that is fine. The only assertions
// are the ones that must hold for any input: Execute returns a result or a Status and never
// crashes, hangs or trips a sanitizer, and a result is a valid Arrow table.

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quarry/engine.h"

namespace {

constexpr int kRows = 24;

// The bytes of one input, read as a stream of choices; an exhausted input always chooses 0, which
// is the shortest alternative of every production, so every input ends.
class Choices {
 public:
  Choices(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  unsigned Pick(unsigned count) { return pos_ < size_ ? data_[pos_++] % count : 0; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;
};

template <typename Builder, typename Fn>
std::shared_ptr<arrow::Array> Build(Fn value_at) {
  Builder builder;
  for (int row = 0; row < kRows; ++row) {
    const auto value = value_at(row);
    if (value.has_value()) {
      if (!builder.Append(*value).ok()) std::abort();
    } else {
      if (!builder.AppendNull().ok()) std::abort();
    }
  }
  std::shared_ptr<arrow::Array> out;
  if (!builder.Finish(&out).ok()) std::abort();
  return out;
}

void WriteTable(const std::filesystem::path& file, int salt) {
  auto i = Build<arrow::Int32Builder>([&](int r) -> std::optional<int32_t> {
    if ((r + salt) % 5 == 0) return std::nullopt;
    return r == 3 ? INT32_MAX : (r + salt) % 7 - 3;
  });
  auto l = Build<arrow::Int64Builder>([&](int r) -> std::optional<int64_t> {
    if ((r + salt) % 6 == 0) return std::nullopt;
    return r == 4 ? INT64_MIN : static_cast<int64_t>((r * 37 + salt) % 11) - 5;
  });
  auto x = Build<arrow::DoubleBuilder>([&](int r) -> std::optional<double> {
    if ((r + salt) % 7 == 0) return std::nullopt;
    return static_cast<double>((r * 13 + salt) % 17 - 8) / 4.0;
  });
  auto t = Build<arrow::Date32Builder>([&](int r) -> std::optional<int32_t> {
    if ((r + salt) % 8 == 0) return std::nullopt;
    return 18000 + (r * 5 + salt) % 40;
  });
  auto s = Build<arrow::StringBuilder>([&](int r) -> std::optional<std::string> {
    if ((r + salt) % 9 == 0) return std::nullopt;
    return std::string(1, static_cast<char>('a' + (r + salt) % 4));
  });
  auto f = Build<arrow::BooleanBuilder>([&](int r) -> std::optional<bool> {
    if ((r + salt) % 10 == 0) return std::nullopt;
    return (r + salt) % 3 == 0;
  });

  const auto d_type = arrow::decimal128(10, 2);
  arrow::Decimal128Builder d_builder(d_type);
  const auto w_type = arrow::decimal128(38, 4);
  arrow::Decimal128Builder w_builder(w_type);
  for (int r = 0; r < kRows; ++r) {
    if ((r + salt) % 4 == 0) {
      if (!d_builder.AppendNull().ok() || !w_builder.AppendNull().ok()) std::abort();
      continue;
    }
    if (!d_builder.Append(arrow::Decimal128((r * 101 + salt) % 2000 - 1000)).ok()) std::abort();
    // The widest value the type holds, and small ones.
    const arrow::Decimal128 wide =
        r == 5 ? arrow::Decimal128::GetMaxValue(38) : arrow::Decimal128((r + salt) % 9 - 4);
    if (!w_builder.Append(wide).ok()) std::abort();
  }
  std::shared_ptr<arrow::Array> d;
  std::shared_ptr<arrow::Array> w;
  if (!d_builder.Finish(&d).ok() || !w_builder.Finish(&w).ok()) std::abort();

  auto schema =
      arrow::schema({arrow::field("i", arrow::int32()), arrow::field("l", arrow::int64()),
                     arrow::field("d", d_type), arrow::field("w", w_type),
                     arrow::field("x", arrow::float64()), arrow::field("t", arrow::date32()),
                     arrow::field("s", arrow::utf8()), arrow::field("f", arrow::boolean())});
  auto table = arrow::Table::Make(schema, {i, l, d, w, x, t, s, f});
  auto out = arrow::io::FileOutputStream::Open(file.string());
  if (!out.ok()) std::abort();
  if (!parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, 8).ok()) std::abort();
  if (!(*out)->Close().ok()) std::abort();
}

// The two tables a and b, written once per process.
const std::filesystem::path& DataDirectory() {
  static const std::filesystem::path directory = [] {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("quarry-fuzz-query-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    WriteTable(path / "a.parquet", 0);
    WriteTable(path / "b.parquet", 2);
    std::atexit([] {
      std::error_code ignored;
      std::filesystem::remove_all(DataDirectory(), ignored);
    });
    return path;
  }();
  return directory;
}

const char* const kNumeric[] = {"i", "l", "d", "w", "x"};
const char* const kAll[] = {"i", "l", "d", "w", "x", "t", "s", "f"};
const char* const kCompare[] = {"=", "<>", "<", "<=", ">", ">="};
const char* const kNumericLiteral[] = {"0",   "1",      "-3",      "2147483648",
                                       "0.5", "-12.25", "100.001", "1.5"};
const char* const kDateLiteral[] = {"DATE '2019-04-14'", "DATE '2019-05-01' + INTERVAL '1' MONTH",
                                    "DATE '2019-06-01' - INTERVAL '10' DAY"};
const char* const kStringLiteral[] = {"''", "'a'", "'b'", "'zz'"};
const char* const kRules[] = {"fold_constants", "push_filters", "prune_row_groups",
                              "prune_columns",  "order_joins",  "pick_build_side"};

class QueryGrammar {
 public:
  explicit QueryGrammar(Choices* choices) : c_(choices) {}

  std::string Query() {
    const bool join = c_->Pick(3) == 0;
    join_ = join;
    const bool aggregate = c_->Pick(3) == 0;
    std::string sql = "SELECT ";
    std::vector<std::string> keys;
    const unsigned items = 1 + c_->Pick(3);
    if (aggregate) {
      const unsigned group_keys = c_->Pick(3);
      for (unsigned k = 0; k < group_keys; ++k) keys.push_back(Column(kAll));
      for (unsigned k = 0; k < keys.size(); ++k)
        sql += keys[k] + " AS g" + std::to_string(k) + ", ";
      for (unsigned k = 0; k < items; ++k) {
        sql += Aggregate() + " AS a" + std::to_string(k) + (k + 1 < items ? ", " : "");
      }
    } else {
      for (unsigned k = 0; k < items; ++k) {
        sql += (c_->Pick(4) == 0 ? Predicate(1) : Numeric(2)) + " AS o" + std::to_string(k) +
               (k + 1 < items ? ", " : "");
      }
    }
    sql += " FROM a";
    if (join) sql += std::string(" JOIN b ON a.") + Pick(kAll, 8) + " = b." + Pick(kAll, 8);
    if (c_->Pick(2) == 0) sql += " WHERE " + Predicate(2);
    if (aggregate && !keys.empty()) {
      sql += " GROUP BY ";
      for (unsigned k = 0; k < keys.size(); ++k) sql += (k ? ", " : "") + keys[k];
      if (c_->Pick(3) == 0) sql += " HAVING " + Aggregate() + " " + Pick(kCompare, 6) + " 1";
    }
    if (c_->Pick(3) == 0) {
      sql += aggregate ? (keys.empty() ? " ORDER BY a0" : " ORDER BY g0") : " ORDER BY o0";
      if (c_->Pick(2) == 0) sql += " DESC";
      if (c_->Pick(2) == 0) sql += " LIMIT " + std::to_string(c_->Pick(30));
    }
    return sql;
  }

 private:
  const char* Pick(const char* const* values, unsigned count) { return values[c_->Pick(count)]; }

  template <size_t N>
  std::string Column(const char* const (&values)[N]) {
    const std::string table = join_ && c_->Pick(2) == 0 ? "b." : "a.";
    return table + values[c_->Pick(N)];
  }

  std::string Aggregate() {
    switch (c_->Pick(6)) {
      case 0:
        return "COUNT(*)";
      case 1:
        return "SUM(" + Column(kNumeric) + ")";
      case 2:
        return "AVG(" + Column(kNumeric) + ")";
      case 3:
        return "MIN(" + Column(kAll) + ")";
      case 4:
        return "MAX(" + Column(kAll) + ")";
      default:
        return "COUNT(" + Column(kAll) + ")";
    }
  }

  std::string Numeric(int depth) {
    const unsigned choice = depth <= 0 ? c_->Pick(3) : c_->Pick(7);
    switch (choice) {
      case 0:
      case 1:
        return Column(kNumeric);
      case 2:
        return Pick(kNumericLiteral, 8);
      case 3:
        return "(" + Numeric(depth - 1) + " + " + Numeric(depth - 1) + ")";
      case 4:
        return "(" + Numeric(depth - 1) + " - " + Numeric(depth - 1) + ")";
      case 5:
        return "(" + Numeric(depth - 1) + " * " + Numeric(depth - 1) + ")";
      default:
        return "-" + Column(kNumeric);
    }
  }

  std::string Predicate(int depth) {
    const unsigned choice = depth <= 0 ? c_->Pick(7) : c_->Pick(10);
    switch (choice) {
      case 0:
      case 1:
        return Numeric(1) + " " + Pick(kCompare, 6) + " " + Numeric(1);
      case 2:
        return Column(kAll) + (c_->Pick(2) == 0 ? " IS NULL" : " IS NOT NULL");
      case 3:
        return "a.t " + std::string(Pick(kCompare, 6)) + " " + Pick(kDateLiteral, 3);
      case 4:
        return "a.s " + std::string(Pick(kCompare, 6)) + " " + Pick(kStringLiteral, 4);
      case 5:
        return Column(kNumeric) + (c_->Pick(2) == 0 ? " NOT" : "") + " BETWEEN " +
               Pick(kNumericLiteral, 8) + " AND " + Pick(kNumericLiteral, 8);
      case 6:
        return Column(kNumeric) + (c_->Pick(2) == 0 ? " NOT" : "") + " IN (" +
               Pick(kNumericLiteral, 8) + ", " + Pick(kNumericLiteral, 8) + ")";
      case 7:
        return "(" + Predicate(depth - 1) + " AND " + Predicate(depth - 1) + ")";
      case 8:
        return "(" + Predicate(depth - 1) + " OR " + Predicate(depth - 1) + ")";
      default:
        return "NOT (" + Predicate(depth - 1) + ")";
    }
  }

  Choices* c_;
  bool join_ = false;
};

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  Choices choices(data, size);

  quarry::EngineOptions options;
  static constexpr int32_t kBatchSizes[] = {1, 3, 2048};
  static constexpr int64_t kMemoryLimits[] = {0, 0, 16 * 1024, 64 * 1024};
  options.batch_size = kBatchSizes[choices.Pick(3)];
  options.memory_limit_bytes = kMemoryLimits[choices.Pick(4)];
  const unsigned rule = choices.Pick(8);
  if (rule < 6) options.disabled_rules.insert(kRules[rule]);
  options.timeout = std::chrono::seconds(5);

  const std::string sql = QueryGrammar(&choices).Query();

  quarry::Engine engine(options);
  if (!engine.RegisterDirectory(DataDirectory()).ok()) __builtin_trap();
  const auto result = engine.Execute(sql);
  if (result.ok()) {
    // A result must be a valid Arrow table; an error is simply a Status.
    if (result->table == nullptr || !result->table->ValidateFull().ok()) __builtin_trap();
  }
  return 0;
}
