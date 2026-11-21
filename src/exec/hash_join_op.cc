#include <arrow/status.h>

#include <algorithm>
#include <functional>
#include <vector>

#include "exec/join_hash_table.h"
#include "exec/operator_util.h"
#include "exec/operators.h"
#include "exec/spill_partitions.h"
#include "expr/eval.h"
#include "vector/gather.h"

namespace quarry {
namespace {

// An inner hash join. The build side goes into a hash table, charged to the memory budget. When it
// no longer fits, the join becomes a grace hash join: the build rows seen so far and all the rest
// are split by the hash of their key into kSpillFanout files, the probe side is split the same way,
// and the pairs of files are joined one at a time. A pair whose build file still does not fit is
// split again on the next bits of the hash (up to kMaxSpillDepth). Rows with a NULL key match
// nothing and are dropped when they are split.
class HashJoinOperator final : public Operator {
 public:
  HashJoinOperator(const PhysicalHashJoin& node, std::unique_ptr<Operator> probe,
                   std::unique_ptr<Operator> build)
      : node_(node), probe_(std::move(probe)), build_(std::move(build)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    reservation_ =
        std::make_unique<Reservation>(*context.budget, ReservationOwner("HashJoin", node_.id));
    charged_ = std::make_unique<ChargedBytes>(reservation_.get());
    table_ = JoinHashTable({}, context.pool);
    ARROW_RETURN_NOT_OK(probe_->Open(context));
    return build_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (!built_) {
      ARROW_RETURN_NOT_OK(Build());
      built_ = true;
    }
    if (!spilled_ && table_.rows() == 0) return std::optional<Batch>();  // joins nothing
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      if (!probing_) {
        ARROW_ASSIGN_OR_RAISE(std::optional<Batch> next, NextProbeBatch());
        if (!next.has_value()) {
          ResetTable();  // the join is over: give the table back to the budget
          return std::optional<Batch>();
        }
        ARROW_ASSIGN_OR_RAISE(probe_batch_, next->Compact(context_->pool));
        std::vector<std::shared_ptr<arrow::Array>> keys;
        for (const ExprPtr& key : node_.probe_keys) {
          ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*key, probe_batch_, context_->pool));
          keys.push_back(std::move(column));
        }
        ARROW_RETURN_NOT_OK(table_.Lookup(keys, probe_batch_.length, &first_));
        row_ = 0;
        match_ = probe_batch_.length > 0 ? first_[0] : JoinHashTable::kNoRow;
        probing_ = true;
      }
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> out, ProduceBatch());
      if (out.has_value()) return out;
    }
  }

  void Close() override {
    probe_->Close();
    build_->Close();
  }

  void FillStats(OperatorStats* stats) const override {
    if (reservation_ != nullptr) stats->peak_state_bytes = reservation_->peak();
    stats->spilled_bytes = spilled_bytes_;
  }

 private:
  // A pair of files holding the build and the probe rows of the keys that share a partition, and
  // how many times their rows have been split by hash already.
  struct Work {
    std::filesystem::path build_path;
    std::filesystem::path probe_path;
    int depth = 0;
  };

  using BatchSource = std::function<arrow::Result<std::optional<Batch>>()>;

  arrow::Status Build() {
    sink_depth_ = 0;
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> next, build_->Next());
      if (!next.has_value()) break;
      ARROW_ASSIGN_OR_RAISE(Batch batch, next->Compact(context_->pool));
      ARROW_RETURN_NOT_OK(IngestBuildBatch(batch));
    }
    if (!spilled_) return ConcatenateBuild();
    // The build side is in its partition files; split the probe side the same way and queue the
    // pairs.
    ARROW_ASSIGN_OR_RAISE(auto probe_sink, SplitProbe([this]() { return probe_->Next(); }, 0));
    return QueuePairs(build_sink_.get(), probe_sink.get(), 0);
  }

  // Puts the rows of `batch` (no selection) into the table, or, once the table has grown past the
  // budget, into the partition files.
  arrow::Status IngestBuildBatch(const Batch& batch) {
    if (batch.length == 0) return arrow::Status::OK();
    std::vector<std::shared_ptr<arrow::Array>> keys;
    for (const ExprPtr& key : node_.build_keys) {
      ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*key, batch, context_->pool));
      keys.push_back(std::move(column));
    }
    if (build_sink_ != nullptr) return SinkBuildRows(batch, keys);
    const int64_t step = std::max<int64_t>(1, StateSliceRows(*context_, batch.length));
    for (int64_t offset = 0; offset < batch.length; offset += step) {
      const int64_t length = std::min(step, batch.length - offset);
      std::vector<std::shared_ptr<arrow::Array>> key_slice;
      key_slice.reserve(keys.size());
      for (const auto& key : keys) key_slice.push_back(key->Slice(offset, length));
      Batch slice = SliceBatch(batch, offset, length);
      // The table's growth is charged before it is allocated: a rehash holds the old slot array
      // and the new one at once. When the budget refuses, the table is left as it was and this
      // slice and the rest of the batch go to the partition files.
      int64_t refused = 0;
      const auto charge = [&](int64_t growth) {
        const int64_t needed = payload_bytes_ + table_.memory_bytes() + growth;
        if (charged_->TrySet(needed)) return true;
        refused = needed;
        return false;
      };
      ARROW_ASSIGN_OR_RAISE(const bool inserted, table_.Insert(key_slice, length, charge));
      if (!inserted) {
        if (context_->spill == nullptr) return charged_->Set(refused);
        ARROW_RETURN_NOT_OK(StartSinking(refused));
        return SinkBuildRows(SliceBatch(batch, offset, batch.length - offset),
                             SliceKeys(keys, offset, batch.length - offset));
      }
      payload_bytes_ += BatchBytes(slice);
      build_parts_.push_back(std::move(slice));
      const int64_t needed = payload_bytes_ + table_.memory_bytes();
      if (charged_->TrySet(needed)) continue;
      // Over budget. Without a spill directory that is an error, as before.
      if (context_->spill == nullptr) return charged_->Set(needed);
      ARROW_RETURN_NOT_OK(StartSinking(needed));
      const int64_t rest = offset + length;
      if (rest >= batch.length) return arrow::Status::OK();
      return SinkBuildRows(SliceBatch(batch, rest, batch.length - rest),
                           SliceKeys(keys, rest, batch.length - rest));
    }
    return arrow::Status::OK();
  }

  static std::vector<std::shared_ptr<arrow::Array>> SliceKeys(
      const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t offset, int64_t length) {
    std::vector<std::shared_ptr<arrow::Array>> out;
    out.reserve(keys.size());
    for (const auto& key : keys) out.push_back(key->Slice(offset, length));
    return out;
  }

  // Turns the table into partition files: writes the build rows held so far (at the depth of the
  // work in progress) and empties the table. `needed` is what the table wanted to hold.
  arrow::Status StartSinking(int64_t needed) {
    if (sink_depth_ > kMaxSpillDepth) {
      return PartitionTooLarge(*context_, reservation_->owner(), needed, sink_depth_ - 1);
    }
    build_sink_ = std::make_unique<SpillPartitions>(
        *context_, SpillSchema(OutputArrowTypes(*node_.build)), "join-build", sink_depth_);
    spilled_ = true;
    for (const Batch& part : build_parts_) {
      std::vector<std::shared_ptr<arrow::Array>> keys;
      for (const ExprPtr& key : node_.build_keys) {
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*key, part, context_->pool));
        keys.push_back(std::move(column));
      }
      ARROW_RETURN_NOT_OK(SinkBuildRows(part, keys));
    }
    ResetTable();
    return arrow::Status::OK();
  }

  arrow::Status SinkBuildRows(const Batch& batch,
                              const std::vector<std::shared_ptr<arrow::Array>>& keys) {
    std::vector<uint64_t> hashes;
    std::vector<uint8_t> any_null;
    ARROW_RETURN_NOT_OK(HashKeyRows(keys, batch.length, &hashes, &any_null));
    return build_sink_->Append(batch, hashes, any_null.data());
  }

  static std::vector<std::shared_ptr<arrow::DataType>> OutputArrowTypes(const PhysicalNode& node) {
    std::vector<std::shared_ptr<arrow::DataType>> types;
    types.reserve(node.output_types.size());
    for (const LogicalType& type : node.output_types) types.push_back(type.ToArrow());
    return types;
  }

  // Splits everything `next` yields by the hash of the probe key, at `depth`. Rows with a NULL key
  // can match nothing and are dropped. The files are finished.
  arrow::Result<std::unique_ptr<SpillPartitions>> SplitProbe(const BatchSource& next, int depth) {
    auto sink = std::make_unique<SpillPartitions>(
        *context_, SpillSchema(OutputArrowTypes(*node_.probe)), "join-probe", depth);
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, next());
      if (!batch.has_value()) break;
      ARROW_ASSIGN_OR_RAISE(Batch compact, batch->Compact(context_->pool));
      if (compact.length == 0) continue;
      std::vector<std::shared_ptr<arrow::Array>> keys;
      for (const ExprPtr& key : node_.probe_keys) {
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*key, compact, context_->pool));
        keys.push_back(std::move(column));
      }
      std::vector<uint64_t> hashes;
      std::vector<uint8_t> any_null;
      ARROW_RETURN_NOT_OK(HashKeyRows(keys, compact.length, &hashes, &any_null));
      ARROW_RETURN_NOT_OK(sink->Append(compact, hashes, any_null.data()));
    }
    const arrow::Status status = sink->Finish();
    spilled_bytes_ += sink->bytes_written();
    ARROW_RETURN_NOT_OK(status);
    return sink;
  }

  // Finishes the build files and queues each partition that has rows on both sides; a partition
  // with rows on one side only joins nothing, and its file is dropped.
  arrow::Status QueuePairs(SpillPartitions* build, SpillPartitions* probe, int depth) {
    const arrow::Status status = build->Finish();
    spilled_bytes_ += build->bytes_written();
    ARROW_RETURN_NOT_OK(status);
    for (int p = 0; p < kSpillFanout; ++p) {
      if (build->rows(p) > 0 && probe->rows(p) > 0) {
        work_.push_back({build->path(p), probe->path(p), depth});
        continue;
      }
      if (build->rows(p) > 0) RemoveSpillFile(build->path(p));
      if (probe->rows(p) > 0) RemoveSpillFile(probe->path(p));
    }
    build_sink_.reset();
    return arrow::Status::OK();
  }

  // The next batch of probe rows: the probe input itself, or, in a spilled join, the file of the
  // partition being joined (loading the next partition when one is used up).
  arrow::Result<std::optional<Batch>> NextProbeBatch() {
    if (!spilled_) return probe_->Next();
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      if (probe_reader_ != nullptr) {
        ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, probe_reader_->Next());
        if (batch.has_value()) return batch;
        probe_reader_.reset();
        RemoveSpillFile(probe_path_);
        ResetTable();
        continue;
      }
      if (work_.empty()) return std::optional<Batch>();
      const Work work = work_.back();
      work_.pop_back();
      ARROW_RETURN_NOT_OK(LoadPartition(work));
    }
  }

  // Builds the table of one partition's build file, to be probed with its probe file. When the
  // build rows do not fit, splits both files on the next bits of the hash instead and queues the
  // pieces.
  arrow::Status LoadPartition(const Work& work) {
    ResetTable();
    sink_depth_ = work.depth + 1;
    ARROW_ASSIGN_OR_RAISE(auto reader, SpillReader::Open(work.build_path, context_->fault));
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, reader->Next());
      if (!batch.has_value()) break;
      ARROW_RETURN_NOT_OK(IngestBuildBatch(*batch));
    }
    reader.reset();
    RemoveSpillFile(work.build_path);
    if (build_sink_ == nullptr) {
      ARROW_RETURN_NOT_OK(ConcatenateBuild());
      ARROW_ASSIGN_OR_RAISE(probe_reader_, SpillReader::Open(work.probe_path, context_->fault));
      probe_path_ = work.probe_path;
      return arrow::Status::OK();
    }
    ARROW_ASSIGN_OR_RAISE(auto probe_reader, SpillReader::Open(work.probe_path, context_->fault));
    ARROW_ASSIGN_OR_RAISE(
        auto probe_sink,
        SplitProbe([&probe_reader]() { return probe_reader->Next(); }, sink_depth_));
    probe_reader.reset();
    RemoveSpillFile(work.probe_path);
    return QueuePairs(build_sink_.get(), probe_sink.get(), sink_depth_);
  }

  void ResetTable() {
    table_ = JoinHashTable({}, context_->pool);
    build_parts_.clear();
    build_columns_.clear();
    payload_bytes_ = 0;
    (void)charged_->TrySet(0);
  }

  // The build rows held in the table, as one array per column.
  arrow::Status ConcatenateBuild() {
    const size_t width = node_.build->output_types.size();
    build_columns_.clear();
    for (size_t c = 0; c < width; ++c) {
      std::vector<std::shared_ptr<arrow::Array>> parts;
      parts.reserve(build_parts_.size());
      for (const Batch& part : build_parts_) parts.push_back(part.columns[c]);
      ARROW_ASSIGN_OR_RAISE(
          auto column, ConcatenateArrays(*context_, parts, node_.build->output_types[c].ToArrow()));
      build_columns_.push_back(std::move(column));
    }
    build_parts_.clear();
    return arrow::Status::OK();
  }

  // Up to batch_size matches of the current probe batch, from where the last call stopped.
  arrow::Result<std::optional<Batch>> ProduceBatch() {
    std::vector<uint32_t> probe_rows;
    std::vector<uint32_t> build_rows;
    const auto limit = static_cast<size_t>(context_->batch_size);
    while (row_ < probe_batch_.length && probe_rows.size() < limit) {
      if (match_ == JoinHashTable::kNoRow) {
        ++row_;
        match_ =
            row_ < probe_batch_.length ? first_[static_cast<size_t>(row_)] : JoinHashTable::kNoRow;
        continue;
      }
      probe_rows.push_back(static_cast<uint32_t>(row_));
      build_rows.push_back(match_);
      match_ = table_.Next(match_);
    }
    if (row_ >= probe_batch_.length) probing_ = false;
    if (probe_rows.empty()) return std::optional<Batch>();

    const auto n = static_cast<int64_t>(probe_rows.size());
    const auto gather_probe = [&](int index) {
      return Gather(*probe_batch_.columns[static_cast<size_t>(index)], probe_rows, context_->pool);
    };
    const auto gather_build = [&](int index) {
      return Gather(*build_columns_[static_cast<size_t>(index)], build_rows, context_->pool);
    };

    if (node_.residual == nullptr) {
      std::vector<std::shared_ptr<arrow::Array>> columns;
      columns.reserve(node_.output_columns.size());
      for (const JoinOutputColumn& out : node_.output_columns) {
        ARROW_ASSIGN_OR_RAISE(auto column,
                              out.from_build ? gather_build(out.index) : gather_probe(out.index));
        columns.push_back(std::move(column));
      }
      return std::optional<Batch>(MakeBatch(std::move(columns), n));
    }

    // The residual sees every probe column and then every build column of the matched pairs.
    std::vector<std::shared_ptr<arrow::Array>> probe_side;
    std::vector<std::shared_ptr<arrow::Array>> build_side;
    std::vector<std::shared_ptr<arrow::Array>> both;
    for (size_t c = 0; c < probe_batch_.columns.size(); ++c) {
      ARROW_ASSIGN_OR_RAISE(auto column, gather_probe(static_cast<int>(c)));
      probe_side.push_back(column);
      both.push_back(std::move(column));
    }
    for (size_t c = 0; c < build_columns_.size(); ++c) {
      ARROW_ASSIGN_OR_RAISE(auto column, gather_build(static_cast<int>(c)));
      build_side.push_back(column);
      both.push_back(std::move(column));
    }
    Batch candidates = MakeBatch(std::move(both), n);
    ARROW_ASSIGN_OR_RAISE(SelectionVector kept,
                          EvaluatePredicate(*node_.residual, candidates, context_->pool));
    if (kept.size() == 0) return std::optional<Batch>();
    std::vector<std::shared_ptr<arrow::Array>> columns;
    columns.reserve(node_.output_columns.size());
    for (const JoinOutputColumn& out : node_.output_columns) {
      columns.push_back(out.from_build ? build_side[static_cast<size_t>(out.index)]
                                       : probe_side[static_cast<size_t>(out.index)]);
    }
    Batch result = MakeBatch(std::move(columns), n);
    if (kept.size() < n) result.selection = std::move(kept);
    return std::optional<Batch>(std::move(result));
  }

  const PhysicalHashJoin& node_;
  std::unique_ptr<Operator> probe_;
  std::unique_ptr<Operator> build_;
  ExecContext* context_ = nullptr;
  std::unique_ptr<Reservation> reservation_;
  std::unique_ptr<ChargedBytes> charged_;
  JoinHashTable table_;
  std::vector<Batch> build_parts_;  // the build rows in the table, until they are concatenated
  int64_t payload_bytes_ = 0;
  std::vector<std::shared_ptr<arrow::Array>> build_columns_;
  bool built_ = false;

  // Spilling: the files being written for the build side (null while the table fits), the depth
  // they are written at, the pairs still to join, and the probe file being read.
  bool spilled_ = false;
  std::unique_ptr<SpillPartitions> build_sink_;
  int sink_depth_ = 0;
  std::vector<Work> work_;
  std::unique_ptr<SpillReader> probe_reader_;
  std::filesystem::path probe_path_;
  int64_t spilled_bytes_ = 0;

  // The probe batch being matched: its first match per row, and where the walk stopped.
  bool probing_ = false;
  Batch probe_batch_;
  std::vector<uint32_t> first_;
  int64_t row_ = 0;
  uint32_t match_ = JoinHashTable::kNoRow;
};

}  // namespace

std::unique_ptr<Operator> MakeHashJoinOperator(const PhysicalHashJoin& node,
                                               std::unique_ptr<Operator> probe,
                                               std::unique_ptr<Operator> build) {
  return std::make_unique<HashJoinOperator>(node, std::move(probe), std::move(build));
}

}  // namespace quarry
