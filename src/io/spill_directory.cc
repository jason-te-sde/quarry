#include "io/spill_directory.h"

#include <arrow/status.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

namespace quarry {

namespace {

constexpr std::string_view kPrefix = "quarry-spill-";

// The pid in a name `quarry-spill-<pid>-XXXXXX`, or 0 for any other name, the pid-less names of
// older versions included.
pid_t OwnerOf(std::string_view name) {
  if (!name.starts_with(kPrefix)) return 0;
  name.remove_prefix(kPrefix.size());
  pid_t pid = 0;
  const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
  if (error != std::errc() || end == name.data() || end == name.data() + name.size() ||
      *end != '-' || pid <= 0) {
    return 0;
  }
  return pid;
}

}  // namespace

void SpillDirectory::RemoveOrphans(const std::filesystem::path& parent) {
  std::error_code error;  // best effort: a directory that cannot be read or removed is left alone
  for (auto it = std::filesystem::directory_iterator(parent, error);
       !error && it != std::filesystem::directory_iterator(); it.increment(error)) {
    const pid_t owner = OwnerOf(it->path().filename().string());
    if (owner == 0 || owner == ::getpid()) continue;
    // ESRCH alone means no such process; EPERM is a live process of another user.
    if (::kill(owner, 0) == 0 || errno != ESRCH) continue;
    std::error_code ignored;
    std::filesystem::remove_all(it->path(), ignored);
  }
}

arrow::Result<std::unique_ptr<SpillDirectory>> SpillDirectory::Create(
    const std::filesystem::path& parent) {
  std::error_code error;
  if (!std::filesystem::is_directory(parent, error)) {
    return arrow::Status::IOError("spill directory parent '", parent.string(),
                                  "' is not a directory");
  }
  RemoveOrphans(parent);
  std::string pattern =
      (parent / (std::string(kPrefix) + std::to_string(::getpid()) + "-XXXXXX")).string();
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
