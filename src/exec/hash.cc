#include "exec/hash.h"

#include <cstring>

namespace quarry {

uint64_t HashInt64(uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

uint64_t HashBytes(std::span<const std::byte> bytes) {
  uint64_t hash =
      0xcbf29ce484222325ULL ^ (static_cast<uint64_t>(bytes.size()) * 0x9e3779b97f4a7c15ULL);
  const std::byte* p = bytes.data();
  size_t left = bytes.size();
  while (left >= 8) {
    uint64_t word;
    std::memcpy(&word, p, 8);
    hash = CombineHashes(hash, HashInt64(word));
    p += 8;
    left -= 8;
  }
  if (left > 0) {
    uint64_t word = 0;
    std::memcpy(&word, p, left);
    hash = CombineHashes(hash, HashInt64(word ^ (static_cast<uint64_t>(left) << 56)));
  }
  return HashInt64(hash);
}

uint64_t CombineHashes(uint64_t seed, uint64_t hash) {
  return seed ^ (hash + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

}  // namespace quarry
