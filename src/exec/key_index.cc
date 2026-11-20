#include "exec/key_index.h"

#include <cstring>

#include "common/flaws.h"
#include "exec/hash.h"

namespace quarry {
namespace {
constexpr size_t kInitialSlots = 16;
}  // namespace

KeyIndex::KeyIndex(KeyHasher hasher)
    : hasher_(std::move(hasher)), slots_(kInitialSlots), mask_(kInitialSlots - 1), offsets_{0} {}

uint64_t KeyIndex::Hash(std::span<const uint8_t> key) const {
  const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(key.data()),
                                         key.size());
  return hasher_ ? hasher_(bytes) : HashBytes(bytes);
}

std::span<const uint8_t> KeyIndex::key(int64_t id) const {
  const auto at = static_cast<size_t>(id);
  return {arena_.data() + offsets_[at], offsets_[at + 1] - offsets_[at]};
}

bool KeyIndex::KeyEquals(int64_t id, std::span<const uint8_t> key) const {
  const std::span<const uint8_t> stored = this->key(id);
  return stored.size() == key.size() &&
         (key.empty() || std::memcmp(stored.data(), key.data(), key.size()) == 0);
}

int64_t KeyIndex::Find(std::span<const uint8_t> key, uint64_t hash) const {
  for (size_t i = hash & mask_;; i = (i + 1) & mask_) {
    const Slot& slot = slots_[i];
    if (slot.id_plus_one == 0) {
      return kAbsent;
    }
    if (slot.hash == hash && KeyEquals(static_cast<int64_t>(slot.id_plus_one) - 1, key)) {
      return static_cast<int64_t>(slot.id_plus_one) - 1;
    }
    if constexpr (kFlawDropHashCollisions) {
      // The defect: the probe gives up at the first occupied slot that is not the key, so a key
      // that collided with an earlier one is never found.
      return kAbsent;
    }
  }
}

int64_t KeyIndex::Insert(std::span<const uint8_t> key, uint64_t hash) {
  if ((static_cast<size_t>(size()) + 1) * 2 > slots_.size()) {
    Grow();
  }
  const int64_t id = size();
  arena_.insert(arena_.end(), key.begin(), key.end());
  offsets_.push_back(arena_.size());
  size_t i = hash & mask_;
  while (slots_[i].id_plus_one != 0) {
    i = (i + 1) & mask_;
  }
  slots_[i] = Slot{hash, static_cast<uint32_t>(id + 1)};
  return id;
}

void KeyIndex::Grow() {
  std::vector<Slot> bigger(slots_.size() * 2);
  const size_t mask = bigger.size() - 1;
  for (const Slot& slot : slots_) {
    if (slot.id_plus_one == 0) continue;
    size_t i = slot.hash & mask;
    while (bigger[i].id_plus_one != 0) {
      i = (i + 1) & mask;
    }
    bigger[i] = slot;
  }
  slots_ = std::move(bigger);
  mask_ = mask;
}

int64_t KeyIndex::memory_bytes() const {
  return static_cast<int64_t>(slots_.capacity() * sizeof(Slot) + arena_.capacity() +
                              offsets_.capacity() * sizeof(uint64_t));
}

}  // namespace quarry
