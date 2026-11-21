#ifndef QUARRY_IO_SPILL_DIRECTORY_H_
#define QUARRY_IO_SPILL_DIRECTORY_H_

#include <arrow/result.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace quarry {

// A scratch directory for the files one query spills. Create makes a new, uniquely named directory
// under `parent` (which must exist); the destructor removes it with everything in it, so a query
// that fails, is cancelled or times out leaves nothing behind as long as its SpillDirectory is
// destroyed.
class SpillDirectory {
 public:
  static arrow::Result<std::unique_ptr<SpillDirectory>> Create(const std::filesystem::path& parent);
  ~SpillDirectory();
  SpillDirectory(const SpillDirectory&) = delete;
  SpillDirectory& operator=(const SpillDirectory&) = delete;

  // A path for a new file in the directory, named from `prefix` and a counter; the file is not
  // created. Safe to call from several threads.
  std::filesystem::path NewFile(std::string_view prefix);
  const std::filesystem::path& path() const { return path_; }

 private:
  explicit SpillDirectory(std::filesystem::path path) : path_(std::move(path)) {}

  std::filesystem::path path_;
  std::atomic<int64_t> counter_{0};
};

}  // namespace quarry

#endif  // QUARRY_IO_SPILL_DIRECTORY_H_
