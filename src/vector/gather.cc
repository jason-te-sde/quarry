#include "vector/gather.h"

#include <arrow/buffer.h>
#include <arrow/status.h>
#include <arrow/util/bit_util.h>
#include <arrow/util/bitmap_ops.h>

#include <cstring>

namespace quarry {
namespace {

using BufferPtr = std::shared_ptr<arrow::Buffer>;

bool BitAt(const uint8_t* bits, int64_t i) {
  return arrow::bit_util::GetBit(bits, static_cast<uint64_t>(i));
}

const uint8_t* DataOrNull(const BufferPtr& buffer) {
  return buffer == nullptr ? nullptr : buffer->data();
}

template <size_t kWidth>
void CopyFixed(const uint8_t* in, std::span<const uint32_t> indices, uint8_t* out) {
  for (size_t i = 0; i < indices.size(); ++i) {
    std::memcpy(out + i * kWidth, in + static_cast<size_t>(indices[i]) * kWidth, kWidth);
  }
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> Gather(const arrow::Array& source,
                                                    std::span<const uint32_t> indices,
                                                    arrow::MemoryPool* pool) {
  const arrow::ArrayData& in = *source.data();
  const int64_t n = static_cast<int64_t>(indices.size());
  for (const uint32_t index : indices) {
    if (static_cast<int64_t>(index) >= source.length()) {
      return arrow::Status::Invalid("gather: index ", index, " is out of range for an array of ",
                                    source.length(), " rows");
    }
  }

  BufferPtr validity;
  int64_t null_count = 0;
  const uint8_t* in_valid = nullptr;
  if (source.null_count() > 0) {
    in_valid = in.buffers[0]->data();
    ARROW_ASSIGN_OR_RAISE(validity, arrow::AllocateBitmap(n, pool));
    uint8_t* out_valid = validity->mutable_data();
    for (int64_t i = 0; i < n; ++i) {
      const bool valid = BitAt(in_valid, in.offset + indices[static_cast<size_t>(i)]);
      arrow::bit_util::SetBitTo(out_valid, i, valid);
      null_count += valid ? 0 : 1;
    }
    if (null_count == 0) {
      validity = nullptr;
    }
  }

  std::vector<BufferPtr> buffers{validity};
  switch (source.type_id()) {
    case arrow::Type::BOOL: {
      ARROW_ASSIGN_OR_RAISE(auto values, arrow::AllocateBitmap(n, pool));
      const uint8_t* in_bits = DataOrNull(in.buffers[1]);
      for (int64_t i = 0; i < n; ++i) {
        arrow::bit_util::SetBitTo(values->mutable_data(), i,
                                  BitAt(in_bits, in.offset + indices[static_cast<size_t>(i)]));
      }
      buffers.push_back(std::move(values));
      break;
    }
    case arrow::Type::INT32:
    case arrow::Type::DATE32: {
      ARROW_ASSIGN_OR_RAISE(auto values, arrow::AllocateBuffer(n * 4, pool));
      CopyFixed<4>(DataOrNull(in.buffers[1]) + in.offset * 4, indices, values->mutable_data());
      buffers.push_back(std::move(values));
      break;
    }
    case arrow::Type::INT64:
    case arrow::Type::DOUBLE:
    case arrow::Type::DECIMAL64: {
      ARROW_ASSIGN_OR_RAISE(auto values, arrow::AllocateBuffer(n * 8, pool));
      CopyFixed<8>(DataOrNull(in.buffers[1]) + in.offset * 8, indices, values->mutable_data());
      buffers.push_back(std::move(values));
      break;
    }
    case arrow::Type::DECIMAL128: {
      ARROW_ASSIGN_OR_RAISE(auto values, arrow::AllocateBuffer(n * 16, pool));
      CopyFixed<16>(DataOrNull(in.buffers[1]) + in.offset * 16, indices, values->mutable_data());
      buffers.push_back(std::move(values));
      break;
    }
    case arrow::Type::STRING: {
      const auto* offsets = reinterpret_cast<const int32_t*>(DataOrNull(in.buffers[1])) + in.offset;
      const uint8_t* bytes = DataOrNull(in.buffers[2]);
      int64_t total = 0;
      for (int64_t i = 0; i < n; ++i) {
        const uint32_t row = indices[static_cast<size_t>(i)];
        if (in_valid == nullptr || BitAt(in_valid, in.offset + row)) {
          total += offsets[row + 1] - offsets[row];
        }
      }
      if (total > INT32_MAX) {
        return arrow::Status::Invalid("gather: the strings of the result need ", total,
                                      " bytes, above the 2 GiB limit of a string array");
      }
      ARROW_ASSIGN_OR_RAISE(auto out_offsets, arrow::AllocateBuffer((n + 1) * 4, pool));
      ARROW_ASSIGN_OR_RAISE(auto out_bytes, arrow::AllocateBuffer(total, pool));
      auto* out_offset_values = reinterpret_cast<int32_t*>(out_offsets->mutable_data());
      int32_t position = 0;
      for (int64_t i = 0; i < n; ++i) {
        out_offset_values[i] = position;
        const uint32_t row = indices[static_cast<size_t>(i)];
        if (in_valid == nullptr || BitAt(in_valid, in.offset + row)) {
          const int32_t length = offsets[row + 1] - offsets[row];
          if (length > 0) {
            std::memcpy(out_bytes->mutable_data() + position, bytes + offsets[row],
                        static_cast<size_t>(length));
          }
          position += length;
        }
      }
      out_offset_values[n] = position;
      buffers.push_back(std::move(out_offsets));
      buffers.push_back(std::move(out_bytes));
      break;
    }
    default:
      return arrow::Status::NotImplemented("gather of ", source.type()->ToString());
  }
  return arrow::MakeArray(arrow::ArrayData::Make(source.type(), n, std::move(buffers), null_count));
}

}  // namespace quarry
