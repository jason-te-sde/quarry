#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

#include "io/parquet_scan.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

void PrintUsage(std::FILE* out) { std::fputs("usage: quarry schema <file.parquet>\n", out); }

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

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "schema") == 0) {
    // The library returns Status, but the standard library allocates, and allocation can throw.
    try {
      return RunSchema(argv[2]);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "quarry: %s\n", e.what());
      return kExitError;
    }
  }
  PrintUsage(stderr);
  return kExitUsage;
}
