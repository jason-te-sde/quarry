#include "exec/key_encoding.h"

#include <arrow/array/builder_binary.h>
#include <arrow/status.h>

#include <cmath>
#include <cstring>

#include "expr/eval_internal.h"

namespace quarry {
namespace {

using eval_internal::BitAt;
using eval_internal::Load;
using eval_internal::ValueBytes;

size_t FixedWidth(arrow::Type::type id) {
  switch (id) {
    case arrow::Type::BOOL:
      return 1;
    case arrow::Type::INT32:
    case arrow::Type::DATE32:
      return 4;
    case arrow::Type::INT64:
    case arrow::Type::DOUBLE:
    case arrow::Type::DECIMAL64:
      return 8;
    case arrow::Type::DECIMAL128:
      return 16;
    default:
      return 0;
  }
}

double Normalize(double v) {
  if (v == 0.0) return 0.0;
  if (std::isnan(v)) return std::nan("");
  return v;
}

}  // namespace

arrow::Status EncodeKeys(std::span<const std::shared_ptr<arrow::Array>> keys, int64_t n,
                         EncodedKeys* out) {
  const auto rows = static_cast<size_t>(n);
  out->any_null.assign(rows, 0);
  out->offsets.assign(rows + 1, 0);
  for (const auto& key : keys) {
    if (key->length() != n) {
      return arrow::Status::Invalid("key column has ", key->length(), " rows, expected ", n);
    }
  }

  // Pass 1: the length of every row's encoding.
  std::vector<uint64_t> length(rows, 0);
  for (const auto& key : keys) {
    const arrow::Array& array = *key;
    const uint8_t* valid = array.null_count() > 0 ? array.null_bitmap_data() : nullptr;
    const size_t width = FixedWidth(array.type_id());
    if (width == 0 && array.type_id() != arrow::Type::STRING) {
      return arrow::Status::Invalid("cannot use ", array.type()->ToString(), " as a key");
    }
    const auto* offsets =
        array.type_id() == arrow::Type::STRING
            ? reinterpret_cast<const int32_t*>(array.data()->buffers[1]->data()) + array.offset()
            : nullptr;
    for (size_t i = 0; i < rows; ++i) {
      if (valid != nullptr && !BitAt(valid, array.offset() + static_cast<int64_t>(i))) {
        length[i] += 1;
      } else if (offsets != nullptr) {
        length[i] += 1 + 4 + static_cast<uint64_t>(offsets[i + 1] - offsets[i]);
      } else {
        length[i] += 1 + width;
      }
    }
  }
  for (size_t i = 0; i < rows; ++i) {
    out->offsets[i + 1] = out->offsets[i] + length[i];
  }
  out->bytes.assign(out->offsets[rows], 0);

  // Pass 2: write column by column, each row at its own cursor.
  std::vector<uint64_t> cursor(out->offsets.begin(), out->offsets.end() - 1);
  uint8_t* base = out->bytes.data();
  for (const auto& key : keys) {
    const arrow::Array& array = *key;
    const uint8_t* valid = array.null_count() > 0 ? array.null_bitmap_data() : nullptr;
    const arrow::Type::type id = array.type_id();
    const size_t width = FixedWidth(id);
    const int64_t offset = array.offset();
    const uint8_t* values = id == arrow::Type::STRING ? nullptr : ValueBytes(array);
    const auto* string_offsets =
        id == arrow::Type::STRING
            ? reinterpret_cast<const int32_t*>(array.data()->buffers[1]->data()) + offset
            : nullptr;
    const uint8_t* string_bytes = id == arrow::Type::STRING && array.data()->buffers[2] != nullptr
                                      ? array.data()->buffers[2]->data()
                                      : nullptr;
    for (size_t i = 0; i < rows; ++i) {
      uint8_t* at = base + cursor[i];
      const auto row = static_cast<int64_t>(i);
      if (valid != nullptr && !BitAt(valid, offset + row)) {
        *at = 0;
        out->any_null[i] = 1;
        cursor[i] += 1;
        continue;
      }
      *at++ = 1;
      if (id == arrow::Type::STRING) {
        const auto len = static_cast<uint32_t>(string_offsets[i + 1] - string_offsets[i]);
        std::memcpy(at, &len, 4);
        if (len > 0) std::memcpy(at + 4, string_bytes + string_offsets[i], len);
        cursor[i] += 1 + 4 + len;
      } else if (id == arrow::Type::BOOL) {
        *at = BitAt(values, offset + row) ? 1 : 0;
        cursor[i] += 2;
      } else if (id == arrow::Type::DOUBLE) {
        const double v = Normalize(Load<double>(values, offset, row));
        std::memcpy(at, &v, 8);
        cursor[i] += 1 + 8;
      } else {
        std::memcpy(at, values + static_cast<size_t>(offset + row) * width, width);
        cursor[i] += 1 + width;
      }
    }
  }
  return arrow::Status::OK();
}

arrow::Result<std::vector<std::shared_ptr<arrow::Array>>> DecodeKeys(
    std::span<const std::shared_ptr<arrow::DataType>> types,
    std::span<const std::span<const uint8_t>> rows, arrow::MemoryPool* pool) {
  const int64_t n = static_cast<int64_t>(rows.size());
  std::vector<size_t> cursor(rows.size(), 0);
  std::vector<std::shared_ptr<arrow::Array>> out;
  for (const auto& type : types) {
    const arrow::Type::type id = type->id();
    if (id == arrow::Type::STRING) {
      arrow::StringBuilder builder(pool);
      for (size_t i = 0; i < rows.size(); ++i) {
        const uint8_t* at = rows[i].data() + cursor[i];
        if (*at == 0) {
          ARROW_RETURN_NOT_OK(builder.AppendNull());
          cursor[i] += 1;
          continue;
        }
        uint32_t len;
        std::memcpy(&len, at + 1, 4);
        ARROW_RETURN_NOT_OK(
            builder.Append(reinterpret_cast<const char*>(at + 5), static_cast<int32_t>(len)));
        cursor[i] += 1 + 4 + len;
      }
      std::shared_ptr<arrow::Array> array;
      ARROW_RETURN_NOT_OK(builder.Finish(&array));
      out.push_back(std::move(array));
      continue;
    }
    const size_t width = FixedWidth(id);
    if (width == 0) {
      return arrow::Status::Invalid("cannot decode a key of type ", type->ToString());
    }
    const bool is_bool = id == arrow::Type::BOOL;
    std::shared_ptr<arrow::Buffer> values;
    if (is_bool) {
      ARROW_ASSIGN_OR_RAISE(values, arrow::AllocateEmptyBitmap(n, pool));
    } else {
      ARROW_ASSIGN_OR_RAISE(values, arrow::AllocateBuffer(n * static_cast<int64_t>(width), pool));
      if (values->size() > 0)
        std::memset(values->mutable_data(), 0, static_cast<size_t>(values->size()));
    }
    ARROW_ASSIGN_OR_RAISE(auto validity, arrow::AllocateEmptyBitmap(n, pool));
    auto* value_bytes = values->mutable_data();
    auto* validity_bytes = validity->mutable_data();
    int64_t nulls = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
      const uint8_t* at = rows[i].data() + cursor[i];
      if (*at == 0) {
        ++nulls;
        cursor[i] += 1;
        continue;
      }
      arrow::bit_util::SetBit(validity_bytes, static_cast<int64_t>(i));
      if (is_bool) {
        if (at[1] != 0) arrow::bit_util::SetBit(value_bytes, static_cast<int64_t>(i));
      } else {
        std::memcpy(value_bytes + i * width, at + 1, width);
      }
      cursor[i] += 1 + width;
    }
    out.push_back(arrow::MakeArray(arrow::ArrayData::Make(
        type, n, {nulls == 0 ? nullptr : std::move(validity), std::move(values)}, nulls)));
  }
  return out;
}

}  // namespace quarry
