#include "exec/spill_partitions.h"

#include <system_error>

#include "exec/hash.h"
#include "exec/key_encoding.h"
#include "exec/operator_util.h"
#include "io/spill_directory.h"
#include "vector/gather.h"

namespace quarry {

std::shared_ptr<arrow::Schema> SpillSchema(
    const std::vector<std::shared_ptr<arrow::DataType>>& types) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(types.size());
  for (size_t i = 0; i < types.size(); ++i) {
    fields.push_back(arrow::field("c" + std::to_string(i), types[i]));
  }
  return arrow::schema(std::move(fields));
}

arrow::Status HashKeyRows(const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t n,
                          std::vector<uint64_t>* hashes, std::vector<uint8_t>* any_null) {
  EncodedKeys encoded;
  ARROW_RETURN_NOT_OK(EncodeKeys(keys, n, &encoded));
  hashes->resize(static_cast<size_t>(n));
  for (int64_t i = 0; i < n; ++i) {
    const std::span<const uint8_t> row = encoded.row(i);
    (*hashes)[static_cast<size_t>(i)] =
        HashBytes({reinterpret_cast<const std::byte*>(row.data()), row.size()});
  }
  *any_null = std::move(encoded.any_null);
  return arrow::Status::OK();
}

SpillPartitions::SpillPartitions(ExecContext& context, std::shared_ptr<arrow::Schema> schema,
                                 std::string prefix, int depth)
    : context_(context), schema_(std::move(schema)), prefix_(std::move(prefix)), depth_(depth) {}

arrow::Status SpillPartitions::Append(const Batch& batch, std::span<const uint64_t> hashes,
                                      const uint8_t* skip) {
  ARROW_RETURN_NOT_OK(CheckInterrupted(context_));
  if (context_.spill == nullptr) {
    return arrow::Status::Invalid("spilling needs a spill directory in the ExecContext");
  }
  std::array<std::vector<uint32_t>, kSpillFanout> indices;
  for (int64_t i = 0; i < batch.length; ++i) {
    const auto at = static_cast<size_t>(i);
    if (skip != nullptr && skip[at] != 0) continue;
    indices[static_cast<size_t>(PartitionOf(hashes[at], depth_))].push_back(
        static_cast<uint32_t>(i));
  }
  for (size_t p = 0; p < kSpillFanout; ++p) {
    if (indices[p].empty()) continue;
    if (writers_[p] == nullptr) {
      paths_[p] = context_.spill->NewFile(prefix_);
      ARROW_ASSIGN_OR_RAISE(writers_[p],
                            SpillWriter::Open(paths_[p], schema_, context_.fault, context_.pool));
    }
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(batch.columns.size());
    for (const auto& column : batch.columns) {
      ARROW_ASSIGN_OR_RAISE(auto part, Gather(*column, indices[p], context_.pool));
      columns.push_back(std::move(part));
    }
    ARROW_RETURN_NOT_OK(
        writers_[p]->Write(MakeBatch(std::move(columns), static_cast<int64_t>(indices[p].size()))));
    rows_[p] += static_cast<int64_t>(indices[p].size());
  }
  return arrow::Status::OK();
}

arrow::Status SpillPartitions::Finish() {
  arrow::Status first_error;
  for (auto& writer : writers_) {
    if (writer == nullptr) continue;
    arrow::Status status = writer->Finish();
    finished_bytes_ += writer->bytes_written();
    writer.reset();
    if (first_error.ok()) first_error = std::move(status);
  }
  return first_error;
}

int64_t SpillPartitions::bytes_written() const {
  int64_t bytes = finished_bytes_;
  for (const auto& writer : writers_) {
    if (writer != nullptr) bytes += writer->bytes_written();
  }
  return bytes;
}

void RemoveSpillFile(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::remove(path, error);
}

}  // namespace quarry
