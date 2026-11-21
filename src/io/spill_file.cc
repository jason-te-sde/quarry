#include "io/spill_file.h"

#include <arrow/array/concatenate.h>
#include <arrow/record_batch.h>
#include <arrow/util/byte_size.h>

#include "common/flaws.h"

namespace quarry {
namespace {

arrow::Status CallFault(const SpillFault& fault, std::string_view site) {
  return fault ? fault(site) : arrow::Status::OK();
}

}  // namespace

arrow::Result<std::unique_ptr<SpillWriter>> SpillWriter::Open(const std::filesystem::path& path,
                                                              std::shared_ptr<arrow::Schema> schema,
                                                              SpillFault fault,
                                                              arrow::MemoryPool* pool) {
  std::unique_ptr<SpillWriter> writer(new SpillWriter());
  writer->schema_ = std::move(schema);
  writer->fault_ = std::move(fault);
  writer->pool_ = pool;
  ARROW_ASSIGN_OR_RAISE(writer->sink_, arrow::io::FileOutputStream::Open(path.string()));
  ARROW_ASSIGN_OR_RAISE(writer->writer_,
                        arrow::ipc::MakeStreamWriter(writer->sink_, writer->schema_));
  writer->pending_.resize(static_cast<size_t>(writer->schema_->num_fields()));
  ARROW_ASSIGN_OR_RAISE(writer->bytes_written_, writer->sink_->Tell());
  return writer;
}

SpillWriter::~SpillWriter() {
  // A writer dropped without Finish (an error path) still closes its file.
  if (!finished_ && writer_ != nullptr) {
    (void)writer_->Close();
    (void)sink_->Close();
  }
}

arrow::Status SpillWriter::Write(const Batch& batch) {
  if (finished_) return arrow::Status::Invalid("write to a finished spill file");
  ARROW_ASSIGN_OR_RAISE(Batch live, batch.Compact(pool_));
  if (live.columns.size() != pending_.size()) {
    return arrow::Status::Invalid("a spill file takes ", pending_.size(), " columns, not ",
                                  live.columns.size());
  }
  if (live.length == 0) return arrow::Status::OK();
  for (size_t c = 0; c < pending_.size(); ++c) {
    ARROW_ASSIGN_OR_RAISE(int64_t bytes, arrow::util::ReferencedBufferSize(*live.columns[c]));
    pending_bytes_ += bytes;
    pending_[c].push_back(live.columns[c]);
  }
  pending_rows_ += live.length;
  if (pending_bytes_ >= kSpillFlushBytes) return Flush();
  return arrow::Status::OK();
}

arrow::Status SpillWriter::Flush() {
  if (pending_rows_ == 0) return arrow::Status::OK();
  ARROW_RETURN_NOT_OK(CallFault(fault_, "spill.write"));
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(pending_.size());
  for (auto& parts : pending_) {
    if (parts.size() == 1) {
      columns.push_back(parts[0]);
    } else {
      ARROW_ASSIGN_OR_RAISE(auto joined, arrow::Concatenate(parts, pool_));
      columns.push_back(std::move(joined));
    }
    parts.clear();
  }
  ARROW_RETURN_NOT_OK(
      writer_->WriteRecordBatch(*arrow::RecordBatch::Make(schema_, pending_rows_, columns)));
  rows_written_ += pending_rows_;
  pending_rows_ = 0;
  pending_bytes_ = 0;
  ARROW_ASSIGN_OR_RAISE(bytes_written_, sink_->Tell());
  return arrow::Status::OK();
}

arrow::Status SpillWriter::Finish() {
  if (finished_) return arrow::Status::Invalid("spill file finished twice");
  finished_ = true;
  ARROW_RETURN_NOT_OK(CallFault(fault_, "spill.finish"));
  if constexpr (kFlawSpillDropsLastBatch) {
    // The defect: the rows still buffered are never written.
  } else {
    ARROW_RETURN_NOT_OK(Flush());
  }
  ARROW_RETURN_NOT_OK(writer_->Close());
  ARROW_ASSIGN_OR_RAISE(bytes_written_, sink_->Tell());
  return sink_->Close();
}

arrow::Result<std::unique_ptr<SpillReader>> SpillReader::Open(const std::filesystem::path& path,
                                                              SpillFault fault,
                                                              arrow::MemoryPool* pool) {
  std::unique_ptr<SpillReader> reader(new SpillReader());
  reader->fault_ = std::move(fault);
  ARROW_ASSIGN_OR_RAISE(reader->source_, arrow::io::ReadableFile::Open(path.string(), pool));
  auto options = arrow::ipc::IpcReadOptions::Defaults();
  options.memory_pool = pool;
  ARROW_ASSIGN_OR_RAISE(reader->reader_,
                        arrow::ipc::RecordBatchStreamReader::Open(reader->source_, options));
  return reader;
}

arrow::Result<std::optional<Batch>> SpillReader::Next() {
  ARROW_RETURN_NOT_OK(CallFault(fault_, "spill.read"));
  std::shared_ptr<arrow::RecordBatch> record;
  ARROW_RETURN_NOT_OK(reader_->ReadNext(&record));
  if (record == nullptr) return std::optional<Batch>();
  Batch batch;
  batch.columns = record->columns();
  batch.length = record->num_rows();
  return std::optional<Batch>(std::move(batch));
}

}  // namespace quarry
