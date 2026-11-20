#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "io/parquet_scan.h"
#include "output_format.h"
#include "quarry/engine.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitInterrupted = 130;

void PrintUsage(std::FILE* out) {
  std::fputs(
      "usage: quarry schema <file.parquet>\n"
      "       quarry query --data <dir> (-c SQL | -f FILE) [options]\n"
      "\n"
      "query options:\n"
      "  --data <dir>            directory of Parquet files: x.parquet is table x, a\n"
      "                          subdirectory of Parquet files x/ is table x\n"
      "  -c SQL                  the query\n"
      "  -f FILE                 read the query from a file\n"
      "  --format table|csv|json the output format (default table)\n"
      "  --memory-limit SIZE     limit for operator state: 512MB, 2GiB, 65536 (bytes);\n"
      "                          KB MB GB are powers of 1000, KiB MiB GiB powers of 1024;\n"
      "                          0 = unlimited (default)\n"
      "  --batch-size N          rows per batch (default 2048)\n"
      "  --disable-rule NAME     switch an optimizer rule off; repeatable. Rules: fold_constants,\n"
      "                          push_filters, prune_row_groups, prune_columns, order_joins,\n"
      "                          pick_build_side\n"
      "  --timeout DURATION      end the query after e.g. 500ms, 30s, 2m (a bare number is\n"
      "                          seconds); 0 = none (default)\n"
      "  --explain               print the plan, do not run the query\n"
      "  --explain-analyze       run the query and print the plan with what each operator did\n"
      "\n"
      "exit status: 0 success, 1 query error, 2 usage error, 130 interrupted (SIGINT)\n",
      out);
}

quarry::Engine* g_engine = nullptr;
volatile std::sig_atomic_t g_interrupted = 0;

// Engine::Cancel is one atomic store, which is safe in a signal handler.
void OnInterrupt(int) {
  g_interrupted = 1;
  if (g_engine != nullptr) g_engine->Cancel();
}

int RunSchema(const char* path) {
  auto info = quarry::ReadParquetMetadata(std::filesystem::path(path));
  if (!info.ok()) {
    std::fprintf(stderr, "quarry: %s\n", info.status().ToString().c_str());
    return kExitError;
  }
  for (const auto& field : info->schema->fields()) {
    std::printf("%s\t%s\n", field->name().c_str(), field->type()->ToString().c_str());
  }
  std::printf("rows\t%lld\n", static_cast<long long>(info->num_rows));
  std::printf("row_groups\t%d\n", info->num_row_groups);
  return kExitOk;
}

// "512MB", "2GiB", "65536": a number and an optional unit.
std::optional<int64_t> ParseSize(const std::string& text) {
  size_t digits = 0;
  while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
  if (digits == 0) return std::nullopt;
  int64_t value = 0;
  if (std::from_chars(text.data(), text.data() + digits, value).ec != std::errc())
    return std::nullopt;
  std::string unit = text.substr(digits);
  std::transform(unit.begin(), unit.end(), unit.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  int64_t scale = 0;
  if (unit.empty() || unit == "B")
    scale = 1;
  else if (unit == "KB")
    scale = 1000;
  else if (unit == "MB")
    scale = int64_t{1000} * 1000;
  else if (unit == "GB")
    scale = 1000LL * 1000 * 1000;
  else if (unit == "KIB")
    scale = 1024;
  else if (unit == "MIB")
    scale = 1024LL * 1024;
  else if (unit == "GIB")
    scale = 1024LL * 1024 * 1024;
  else
    return std::nullopt;
  if (value > INT64_MAX / scale) return std::nullopt;
  return value * scale;
}

// "500ms", "30s", "2m", "1h"; a bare number is seconds.
std::optional<std::chrono::milliseconds> ParseDuration(const std::string& text) {
  size_t digits = 0;
  while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
  if (digits == 0) return std::nullopt;
  int64_t value = 0;
  if (std::from_chars(text.data(), text.data() + digits, value).ec != std::errc())
    return std::nullopt;
  const std::string unit = text.substr(digits);
  int64_t scale = 0;
  if (unit == "ms")
    scale = 1;
  else if (unit.empty() || unit == "s")
    scale = 1000;
  else if (unit == "m")
    scale = int64_t{60} * 1000;
  else if (unit == "h")
    scale = int64_t{60} * 60 * 1000;
  else
    return std::nullopt;
  if (value > INT64_MAX / scale) return std::nullopt;
  return std::chrono::milliseconds(value * scale);
}

int Usage(const std::string& message) {
  std::fprintf(stderr, "quarry: %s\n\n", message.c_str());
  PrintUsage(stderr);
  return kExitUsage;
}

int RunQuery(int argc, char** argv) {
  std::string data;
  std::optional<std::string> sql;
  std::optional<std::string> file;
  std::string format = "table";
  quarry::EngineOptions options;
  bool explain = false;
  bool analyze = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto value = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "quarry: %s needs a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "--data" || arg == "-c" || arg == "-f" || arg == "--format" ||
        arg == "--memory-limit" || arg == "--batch-size" || arg == "--disable-rule" ||
        arg == "--timeout") {
      const char* v = value(arg.c_str());
      if (v == nullptr) return kExitUsage;
      if (arg == "--data") {
        data = v;
      } else if (arg == "-c") {
        sql = v;
      } else if (arg == "-f") {
        file = v;
      } else if (arg == "--format") {
        format = v;
        if (format != "table" && format != "csv" && format != "json") {
          return Usage("--format must be table, csv or json, not '" + format + "'");
        }
      } else if (arg == "--memory-limit") {
        const auto size = ParseSize(v);
        if (!size) return Usage(std::string("invalid --memory-limit '") + v + "'");
        options.memory_limit_bytes = *size;
      } else if (arg == "--batch-size") {
        int64_t n = 0;
        const std::string text = v;
        if (std::from_chars(text.data(), text.data() + text.size(), n).ec != std::errc() ||
            n <= 0 || n > 1 << 24) {
          return Usage("--batch-size must be a number from 1 to 16777216, not '" + text + "'");
        }
        options.batch_size = static_cast<int32_t>(n);
      } else if (arg == "--disable-rule") {
        options.disabled_rules.insert(v);
      } else {
        const auto duration = ParseDuration(v);
        if (!duration) return Usage(std::string("invalid --timeout '") + v + "'");
        options.timeout = *duration;
      }
    } else if (arg == "--explain") {
      explain = true;
    } else if (arg == "--explain-analyze") {
      explain = true;
      analyze = true;
    } else if (arg == "--help" || arg == "-h") {
      PrintUsage(stdout);
      return kExitOk;
    } else {
      return Usage("unknown option '" + arg + "'");
    }
  }
  if (data.empty()) return Usage("--data <dir> is required");
  if (sql.has_value() == file.has_value())
    return Usage("give the query with exactly one of -c and -f");
  if (file) {
    std::ifstream in(*file, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "quarry: cannot read %s\n", file->c_str());
      return kExitError;
    }
    sql = std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  quarry::Engine engine(options);
  if (const arrow::Status st = engine.RegisterDirectory(data); !st.ok()) {
    std::fprintf(stderr, "quarry: %s\n", st.ToString().c_str());
    return kExitError;
  }
  g_engine = &engine;
  std::signal(SIGINT, OnInterrupt);

  int code = kExitOk;
  if (explain) {
    auto text = engine.Explain(*sql, analyze);
    if (text.ok()) {
      std::fputs(text->c_str(), stdout);
    } else {
      std::fprintf(stderr, "quarry: %s\n", text.status().message().c_str());
      code = text.status().IsCancelled() && g_interrupted != 0 ? kExitInterrupted : kExitError;
    }
  } else {
    auto result = engine.Execute(*sql);
    if (result.ok()) {
      const std::string text = format == "json"  ? quarry::ToJson(*result->table) + "\n"
                               : format == "csv" ? quarry::ToCsv(*result->table)
                                                 : quarry::ToTable(*result->table);
      std::fputs(text.c_str(), stdout);
    } else {
      std::fprintf(stderr, "quarry: %s\n", result.status().message().c_str());
      code = result.status().IsCancelled() && g_interrupted != 0 ? kExitInterrupted : kExitError;
    }
  }
  g_engine = nullptr;
  return code;
}

}  // namespace

int main(int argc, char** argv) {
  // The library returns Status, but the standard library allocates, and allocation can throw.
  try {
    if (argc == 3 && std::strcmp(argv[1], "schema") == 0) return RunSchema(argv[2]);
    if (argc >= 2 && std::strcmp(argv[1], "query") == 0) return RunQuery(argc, argv);
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
      PrintUsage(stdout);
      return kExitOk;
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "quarry: %s\n", e.what());
    return kExitError;
  }
  PrintUsage(stderr);
  return kExitUsage;
}
