#ifndef QUARRY_VECTOR_GATHER_H_
#define QUARRY_VECTOR_GATHER_H_

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <span>

namespace quarry {

// A new array whose row i is row indices[i] of `source`, NULLs included. Works for BOOLEAN,
// INTEGER, BIGINT, DECIMAL (64 and 128 bit), DOUBLE, DATE and VARCHAR. Invalid when an index is
// out of range.
arrow::Result<std::shared_ptr<arrow::Array>> Gather(const arrow::Array& source,
                                                    std::span<const uint32_t> indices,
                                                    arrow::MemoryPool* pool);

}  // namespace quarry

#endif  // QUARRY_VECTOR_GATHER_H_
