#include "exec/join_hash_table.h"

#include "exec/key_encoding.h"

namespace quarry {

JoinHashTable::JoinHashTable(KeyHasher hasher) : index_(std::move(hasher)) {}

arrow::Status JoinHashTable::Insert(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                    int64_t n) {
  if (rows() + n >= static_cast<int64_t>(kNoRow)) {
    return arrow::Status::Invalid("a hash join build side is limited to ", kNoRow - 1, " rows");
  }
  EncodedKeys encoded;
  ARROW_RETURN_NOT_OK(EncodeKeys(keys, n, &encoded));
  for (int64_t i = 0; i < n; ++i) {
    const auto row = static_cast<uint32_t>(next_.size());
    next_.push_back(kNoRow);
    if (encoded.any_null[static_cast<size_t>(i)] != 0) {
      continue;  // a NULL key matches nothing
    }
    const std::span<const uint8_t> key = encoded.row(i);
    const uint64_t hash = index_.Hash(key);
    const int64_t id = index_.Find(key, hash);
    if (id == KeyIndex::kAbsent) {
      index_.Insert(key, hash);
      head_.push_back(row);
      tail_.push_back(row);
    } else {
      next_[tail_[static_cast<size_t>(id)]] = row;
      tail_[static_cast<size_t>(id)] = row;
    }
  }
  return arrow::Status::OK();
}

arrow::Status JoinHashTable::Lookup(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                    int64_t n, std::vector<uint32_t>* first) const {
  EncodedKeys encoded;
  ARROW_RETURN_NOT_OK(EncodeKeys(keys, n, &encoded));
  first->assign(static_cast<size_t>(n), kNoRow);
  for (int64_t i = 0; i < n; ++i) {
    if (encoded.any_null[static_cast<size_t>(i)] != 0) continue;
    const std::span<const uint8_t> key = encoded.row(i);
    const int64_t id = index_.Find(key, index_.Hash(key));
    if (id != KeyIndex::kAbsent) {
      (*first)[static_cast<size_t>(i)] = head_[static_cast<size_t>(id)];
    }
  }
  return arrow::Status::OK();
}

arrow::Status JoinHashTable::Probe(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                   int64_t n, std::vector<uint32_t>* probe_rows,
                                   std::vector<uint32_t>* build_rows) const {
  std::vector<uint32_t> first;
  ARROW_RETURN_NOT_OK(Lookup(keys, n, &first));
  for (int64_t i = 0; i < n; ++i) {
    for (uint32_t row = first[static_cast<size_t>(i)]; row != kNoRow; row = next_[row]) {
      probe_rows->push_back(static_cast<uint32_t>(i));
      build_rows->push_back(row);
    }
  }
  return arrow::Status::OK();
}

int64_t JoinHashTable::memory_bytes() const {
  return index_.memory_bytes() +
         static_cast<int64_t>((head_.capacity() + tail_.capacity() + next_.capacity()) *
                              sizeof(uint32_t));
}

}  // namespace quarry
