#ifndef QUARRY_EXEC_JOIN_HASH_TABLE_H_
#define QUARRY_EXEC_JOIN_HASH_TABLE_H_

#include <arrow/array.h>
#include <arrow/status.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include "exec/key_index.h"
#include "exec/pool_vector.h"

namespace quarry {

// The build side of a hash join: build rows grouped by their key. Build rows are numbered from 0 in
// the order they are inserted (a row whose key has a NULL is numbered too, but can never be
// matched). Rows with equal keys form a chain that is walked in insertion order.
class JoinHashTable {
 public:
  static constexpr uint32_t kNoRow = UINT32_MAX;

  // The key index, the chains and the key bytes are allocated from `pool`.
  explicit JoinHashTable(KeyHasher hasher = {},
                         arrow::MemoryPool* pool = arrow::default_memory_pool());

  // Asked for the bytes of new storage an insert is about to allocate; false refuses them.
  using ChargeFn = std::function<bool(int64_t bytes)>;

  // Adds `n` build rows, given by their key columns. With `charge`, the storage the rows need is
  // announced to it before it is allocated; when it refuses, nothing is inserted and the result is
  // false. True when the rows are in.
  arrow::Result<bool> Insert(const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t n,
                             const ChargeFn& charge = {});

  // The number of build rows inserted so far.
  int64_t rows() const { return static_cast<int64_t>(next_.size()); }

  // For each of `n` probe rows, the first build row with an equal key, or kNoRow (also when the
  // probe key has a NULL). Next walks on to the following build row of the same key.
  arrow::Status Lookup(const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t n,
                       std::vector<uint32_t>* first) const;
  uint32_t Next(uint32_t build_row) const { return next_[build_row]; }

  // Every (probe row, build row) pair with equal keys, probe rows in order.
  arrow::Status Probe(const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t n,
                      std::vector<uint32_t>* probe_rows, std::vector<uint32_t>* build_rows) const;

  int64_t memory_bytes() const;

 private:
  KeyIndex index_;
  PoolVector<uint32_t> head_;  // per distinct key
  PoolVector<uint32_t> tail_;
  PoolVector<uint32_t> next_;  // per build row
};

}  // namespace quarry

#endif  // QUARRY_EXEC_JOIN_HASH_TABLE_H_
