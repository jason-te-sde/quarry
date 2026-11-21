#include <sys/resource.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
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
      "                          seconds); 0 = none (default). Checked between batches, not\n"
      "                          inside one row group read: swapping can overrun it\n"
      "  --repeat N              run the query once to warm up and N times to measure, in this\n"
      "                          process, and print nothing; needs --timing-json\n"
      "  --timing-json PATH      with --repeat: write the time of each run (SQL text to a\n"
      "                          materialised table), the load average before it, the peak RSS\n"
      "                          and the memory pools' peaks, as JSON\n"
      "  --load-limit LOAD       with --repeat: before each run wait while the 1-minute load\n"
      "                          average is above LOAD (default: do not wait)\n"
      "  --load-wait SECONDS     with --load-limit: wait at most this long (default 120)\n"
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

// The process's peak resident set size in bytes (ru_maxrss is bytes on macOS, KiB elsewhere).
int64_t PeakRssBytes() {
  struct rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#ifdef __APPLE__
  return static_cast<int64_t>(usage.ru_maxrss);
#else
  return static_cast<int64_t>(usage.ru_maxrss) * 1024;
#endif
}

std::string JsonString(const std::string& text) {
  std::string out = "\"";
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += raw;
    } else if (c < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\u%04x", c);
      out += buf;
    } else {
      out += raw;
    }
  }
  return out + "\"";
}

struct Timing {
  int64_t ns = 0;
  double load_1m = 0;
  double waited_s = 0;
};

// The 1-minute load average; with a limit, waits (one second at a time) while it is above the
// limit, for at most `wait_s` seconds. Returns the load seen when it stopped looking.
Timing WaitForLoad(double limit, int64_t wait_s) {
  Timing t;
  const auto start = std::chrono::steady_clock::now();
  for (;;) {
    double load[1] = {0};
    if (getloadavg(load, 1) != 1) load[0] = 0;
    t.load_1m = load[0];
    t.waited_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (limit <= 0 || load[0] <= limit || t.waited_s >= static_cast<double>(wait_s)) return t;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

std::string TimingJson(const Timing& t) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "{\"ns\":%lld,\"load_1m\":%.2f,\"waited_s\":%.1f}",
                static_cast<long long>(t.ns), t.load_1m, t.waited_s);
  return buf;
}

struct RepeatOptions {
  int repeat = 0;
  std::string timing_json;
  double load_limit = 0;
  int64_t load_wait_s = 120;
};

// One warm-up and `repeat` measured executions of the same query in this process. The time of a run
// is from the SQL text in hand to the whole result materialised as a table: the clock stops before
// the table is released. The JSON is written whether the runs succeed or not; a failure is its
// "error" and the exit status is 1. A warm-up that failed is `"warmup":null`, not a zero time.
int RunRepeated(quarry::Engine& engine, const std::string& sql, const RepeatOptions& opts) {
  const int64_t idle_rss = PeakRssBytes();
  std::vector<Timing> runs;
  Timing warmup;
  bool have_warmup = false;  // false when the warm-up itself failed: its time is then `null`
  int64_t pool_peak = 0;
  int64_t scan_pool_peak = 0;
  int64_t spilled = 0;
  int64_t rows = 0;
  std::string error;
  bool timed_out = false;
  for (int i = 0; i <= opts.repeat && error.empty(); ++i) {
    Timing t = WaitForLoad(opts.load_limit, opts.load_wait_s);
    const auto start = std::chrono::steady_clock::now();
    auto result = engine.Execute(sql);
    const auto end = std::chrono::steady_clock::now();
    t.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    if (!result.ok()) {
      error = result.status().message();
      timed_out = result.status().IsCancelled();
      break;
    }
    pool_peak = std::max(pool_peak, result->pool_peak_bytes);
    scan_pool_peak = std::max(scan_pool_peak, result->scan_pool_peak_bytes);
    spilled = 0;
    for (const auto& op : result->profile) spilled += op.spilled_bytes;
    rows = result->table->num_rows();
    if (i == 0) {
      warmup = t;
      have_warmup = true;
    } else {
      runs.push_back(t);
    }
  }

  std::string json = "{";
  if (!error.empty()) {
    json +=
        "\"error\":" + JsonString(error) + ",\"timed_out\":" + (timed_out ? "true" : "false") + ",";
  }
  json += "\"warmup\":" + (have_warmup ? TimingJson(warmup) : std::string("null")) + ",\"runs\":[";
  for (size_t i = 0; i < runs.size(); ++i) json += (i ? "," : "") + TimingJson(runs[i]);
  char tail[320];
  std::snprintf(tail, sizeof(tail),
                "],\"idle_rss_bytes\":%lld,\"peak_rss_bytes\":%lld,\"pool_peak_bytes\":%lld,"
                "\"scan_pool_peak_bytes\":%lld,\"spilled_bytes\":%lld,\"rows\":%lld}\n",
                static_cast<long long>(idle_rss), static_cast<long long>(PeakRssBytes()),
                static_cast<long long>(pool_peak), static_cast<long long>(scan_pool_peak),
                static_cast<long long>(spilled), static_cast<long long>(rows));
  json += tail;
  std::ofstream out(opts.timing_json, std::ios::binary);
  out << json;
  out.close();
  if (!out) {
    std::fprintf(stderr, "quarry: cannot write %s\n", opts.timing_json.c_str());
    return kExitError;
  }
  if (!error.empty()) {
    std::fprintf(stderr, "quarry: %s\n", error.c_str());
    return kExitError;
  }
  return kExitOk;
}

int RunQuery(int argc, char** argv) {
  std::string data;
  std::optional<std::string> sql;
  std::optional<std::string> file;
  std::string format = "table";
  quarry::EngineOptions options;
  bool explain = false;
  bool analyze = false;
  RepeatOptions repeat;

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
        arg == "--timeout" || arg == "--repeat" || arg == "--timing-json" ||
        arg == "--load-limit" || arg == "--load-wait") {
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
      } else if (arg == "--repeat") {
        const std::string text = v;
        int64_t n = 0;
        if (std::from_chars(text.data(), text.data() + text.size(), n).ec != std::errc() ||
            n <= 0 || n > 1000) {
          return Usage("--repeat must be a number from 1 to 1000, not '" + text + "'");
        }
        repeat.repeat = static_cast<int>(n);
      } else if (arg == "--timing-json") {
        repeat.timing_json = v;
      } else if (arg == "--load-limit" || arg == "--load-wait") {
        char* end = nullptr;
        const double number = std::strtod(v, &end);
        if (end == v || *end != '\0' || number < 0) {
          return Usage("invalid " + arg + " '" + v + "'");
        }
        if (arg == "--load-limit") {
          repeat.load_limit = number;
        } else {
          repeat.load_wait_s = static_cast<int64_t>(number);
        }
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
  if ((repeat.repeat > 0) != !repeat.timing_json.empty())
    return Usage("--repeat and --timing-json go together");
  if (repeat.repeat > 0 && explain) return Usage("--repeat cannot be combined with --explain");
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
  if (repeat.repeat > 0) {
    code = RunRepeated(engine, *sql, repeat);
  } else if (explain) {
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
