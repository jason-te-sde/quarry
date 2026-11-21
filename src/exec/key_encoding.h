#ifndef QUARRY_EXEC_KEY_ENCODING_H_
#define QUARRY_EXEC_KEY_ENCODING_H_

// Hash joins and aggregation compare keys as bytes. A row's key columns are written one after the
// other, each as a validity byte (0 for NULL, then nothing more; 1 for a value) followed by the
// value: fixed-width values in their native width, a string as a 4-byte length and its bytes. Two
// rows have the same key exactly when their encodings are equal, which also makes NULLs equal to
// each other (a join drops those rows first). DOUBLE zero is written as +0 and every NaN as one
// NaN, so that the bytes agree with the SQL notion of equal.

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace quarry {

struct EncodedKeys {
  std::vector<uint8_t> bytes;
  std::vector<uint64_t> offsets;  // rows + 1 entries; row i is bytes[offsets[i], offsets[i + 1])
  std::vector<uint8_t> any_null;  // per row: 1 when a key column is NULL

  int64_t rows() const { return static_cast<int64_t>(any_null.size()); }
  std::span<const uint8_t> row(int64_t i) const {
    const auto at = static_cast<size_t>(i);
    return {bytes.data() + offsets[at], offsets[at + 1] - offsets[at]};
  }
};

// Encodes `n` rows of the key columns. Each array must have `n` rows. Invalid for a type that is
// not one of the engine's.
arrow::Status EncodeKeys(std::span<const std::shared_ptr<arrow::Array>> keys, int64_t n,
                         EncodedKeys* out);

// The inverse: rows[i] is one encoded key made of columns of `types`; the result has one array per
// type, with rows.size() rows.
arrow::Result<std::vector<std::shared_ptr<arrow::Array>>> DecodeKeys(
    std::span<const std::shared_ptr<arrow::DataType>> types,
    std::span<const std::span<const uint8_t>> rows, arrow::MemoryPool* pool);

}  // namespace quarry

#endif  // QUARRY_EXEC_KEY_ENCODING_H_
