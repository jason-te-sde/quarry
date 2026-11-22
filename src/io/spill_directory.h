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
//
// The name carries the creating process's pid, `quarry-spill-<pid>-XXXXXX`. A process that dies
// without running the destructor (SIGKILL, a crash, a second Ctrl-C) leaves its directory behind;
// Create removes every such directory under `parent` whose process no longer exists: an entry that
// is a real directory (not a symlink or a file), whose name is exactly that pattern, and whose pid
// `kill(pid, 0)` reports as ESRCH. Removal is best effort and never fails Create.
//
// A pid means something only within one PID namespace on one host. A spill parent must therefore
// not be shared between PID namespaces (containers that bind-mount the same directory) or between
// hosts (a network filesystem): a live peer's directory there looks like a dead process's and is
// removed. The peer's query then fails with an IOError the next time it opens a spill file, either
// to create one or to read one back; it does not return wrong results. Give each such process its
// own `spill_parent`. The default, the system temporary directory (`$TMPDIR`, else `/tmp`), is
// local to the host on a normal setup and is not per user on Linux, where `/tmp` is shared by all
// users of the host; it is shared with containers that bind-mount it, and across hosts if
// `$TMPDIR` points at a shared filesystem.
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

  // Removes the directories under `parent` left by processes that no longer exist. Best effort;
  // only Create calls it.
  static void RemoveOrphans(const std::filesystem::path& parent);

  std::filesystem::path path_;
  std::atomic<int64_t> counter_{0};
};

}  // namespace quarry

#endif  // QUARRY_IO_SPILL_DIRECTORY_H_
