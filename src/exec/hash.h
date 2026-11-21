#ifndef QUARRY_EXEC_HASH_H_
#define QUARRY_EXEC_HASH_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace quarry {

// Hash functions with fixed seeds: the same input hashes to the same value in every process, so a
// run can be reproduced.

// A finalizer with full avalanche (the splitmix64 one).
uint64_t HashInt64(uint64_t value);

// Hashes any byte string, eight bytes at a time.
uint64_t HashBytes(std::span<const std::byte> bytes);

// Mixes two hashes into one; not symmetric.
uint64_t CombineHashes(uint64_t seed, uint64_t hash);

}  // namespace quarry

#endif  // QUARRY_EXEC_HASH_H_
