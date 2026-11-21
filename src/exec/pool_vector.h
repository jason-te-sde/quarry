#ifndef QUARRY_EXEC_POOL_VECTOR_H_
#define QUARRY_EXEC_POOL_VECTOR_H_

// std::vector storage that comes from an arrow::MemoryPool, so that the query's pool measures the
// hash tables' state (key index, key arena, aggregate states, join chains) along with every Arrow
// buffer. A pool that cannot allocate surfaces as std::bad_alloc, as it does for std::vector.
//
// Capacity is grown by one rule, GrownCapacity, and only through ReserveFor. That lets a table say
// beforehand, with GrowthBytes, how many bytes the next growth will allocate (while the old storage
// is still alive), so that the memory budget can be charged before the allocation, not after.

#include <arrow/memory_pool.h>
#include <arrow/stl_allocator.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace quarry {

template <typename T>
using PoolVector = std::vector<T, arrow::stl::allocator<T>>;

template <typename T>
PoolVector<T> MakePoolVector(arrow::MemoryPool* pool) {
  return PoolVector<T>(arrow::stl::allocator<T>(pool));
}

// The capacity a vector of `capacity` takes when it must hold `needed` elements.
inline size_t GrownCapacity(size_t capacity, size_t needed) {
  return needed <= capacity ? capacity : std::max(needed, capacity * 2);
}

template <typename T>
void ReserveFor(PoolVector<T>& vector, size_t needed) {
  if (needed > vector.capacity()) vector.reserve(GrownCapacity(vector.capacity(), needed));
}

// Resizes with the capacity rule above, so that it allocates what GrowthBytes said it would.
template <typename T>
void ResizeFor(PoolVector<T>& vector, size_t size, const T& fill) {
  ReserveFor(vector, size);
  vector.resize(size, fill);
}

// The bytes ReserveFor(vector, needed) would allocate.
template <typename T>
int64_t GrowthBytes(const PoolVector<T>& vector, size_t needed) {
  return needed > vector.capacity()
             ? static_cast<int64_t>(GrownCapacity(vector.capacity(), needed) * sizeof(T))
             : 0;
}

}  // namespace quarry

#endif  // QUARRY_EXEC_POOL_VECTOR_H_
