#include "io/spill_directory.h"

#include <arrow/status.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include <unistd.h>
#include <system_error>

namespace quarry {

arrow::Result<std::unique_ptr<SpillDirectory>> SpillDirectory::Create(
    const std::filesystem::path& parent) {
  std::error_code error;
  if (!std::filesystem::is_directory(parent, error)) {
    return arrow::Status::IOError("spill directory parent '", parent.string(),
                                  "' is not a directory");
  }
  std::string pattern = (parent / "quarry-spill-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    return arrow::Status::IOError("cannot create a spill directory under '", parent.string(),
                                  "': ", std::generic_category().message(errno));
  }
  return std::unique_ptr<SpillDirectory>(new SpillDirectory(std::filesystem::path(pattern)));
}

SpillDirectory::~SpillDirectory() {
  std::error_code error;  // a destructor cannot report; leftovers are a temp-dir matter
  std::filesystem::remove_all(path_, error);
}

std::filesystem::path SpillDirectory::NewFile(std::string_view prefix) {
  const int64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
  return path_ / (std::string(prefix) + "-" + std::to_string(n) + ".spill");
}

}  // namespace quarry
