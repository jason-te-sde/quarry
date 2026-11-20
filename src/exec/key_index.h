#ifndef QUARRY_EXEC_KEY_INDEX_H_
#define QUARRY_EXEC_KEY_INDEX_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace quarry {

// Hashes one encoded key (see key_encoding.h). Tests inject a constant to force every key to
// collide.
using KeyHasher = std::function<uint64_t(std::span<const std::byte>)>;

// An open-addressing hash set of encoded keys with linear probing. Every distinct key gets a dense
// id, in order of first insertion. Slots store the full 64-bit hash, so that a probe compares keys
// only when the hashes are equal, and the table grows without rehashing any key.
class KeyIndex {
 public:
  static constexpr int64_t kAbsent = -1;

  explicit KeyIndex(KeyHasher hasher = {});

  int64_t size() const { return static_cast<int64_t>(offsets_.size()) - 1; }
  uint64_t Hash(std::span<const uint8_t> key) const;

  // The id of `key`, or kAbsent.
  int64_t Find(std::span<const uint8_t> key, uint64_t hash) const;
  // Adds a key that Find did not find and returns its id.
  int64_t Insert(std::span<const uint8_t> key, uint64_t hash);

  std::span<const uint8_t> key(int64_t id) const;
  // Bytes held by the index (slots, keys and offsets).
  int64_t memory_bytes() const;

 private:
  struct Slot {
    uint64_t hash = 0;
    uint32_t id_plus_one = 0;  // 0 marks an empty slot
  };

  void Grow();
  bool KeyEquals(int64_t id, std::span<const uint8_t> key) const;

  KeyHasher hasher_;
  std::vector<Slot> slots_;
  size_t mask_ = 0;
  std::vector<uint8_t> arena_;
  std::vector<uint64_t> offsets_;  // size() + 1 entries
};

}  // namespace quarry

#endif  // QUARRY_EXEC_KEY_INDEX_H_
