#include "exec/key_index.h"

#include <cstring>

#include "common/flaws.h"
#include "exec/hash.h"

namespace quarry {
namespace {
constexpr size_t kInitialSlots = 16;
}  // namespace

KeyIndex::KeyIndex(KeyHasher hasher, arrow::MemoryPool* pool)
    : hasher_(std::move(hasher)),
      slots_(kInitialSlots, Slot{}, arrow::stl::allocator<Slot>(pool)),
      mask_(kInitialSlots - 1),
      arena_(MakePoolVector<uint8_t>(pool)),
      offsets_(MakePoolVector<uint64_t>(pool)) {
  offsets_.push_back(0);
}

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

// The number of slots that keeps the table at most half full with `keys` keys in it.
size_t KeyIndex::SlotsFor(int64_t keys) const {
  size_t slots = slots_.size();
  while (static_cast<size_t>(keys) * 2 > slots) slots *= 2;
  return slots;
}

int64_t KeyIndex::GrowthBytes(int64_t keys, int64_t key_bytes) const {
  int64_t bytes = 0;
  const size_t slots = SlotsFor(size() + keys);
  if (slots > slots_.size()) bytes += static_cast<int64_t>(slots * sizeof(Slot));
  bytes += quarry::GrowthBytes(arena_, arena_.size() + static_cast<size_t>(key_bytes));
  bytes += quarry::GrowthBytes(offsets_, offsets_.size() + static_cast<size_t>(keys));
  return bytes;
}

void KeyIndex::Reserve(int64_t keys, int64_t key_bytes) {
  const size_t slots = SlotsFor(size() + keys);
  if (slots > slots_.size()) Rehash(slots);
  ReserveFor(arena_, arena_.size() + static_cast<size_t>(key_bytes));
  ReserveFor(offsets_, offsets_.size() + static_cast<size_t>(keys));
}

int64_t KeyIndex::Insert(std::span<const uint8_t> key, uint64_t hash) {
  // A caller that did not Reserve gets the same growth, one key at a time.
  Reserve(1, static_cast<int64_t>(key.size()));
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

void KeyIndex::Rehash(size_t slots) {
  PoolVector<Slot> bigger(slots, Slot{}, slots_.get_allocator());
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
