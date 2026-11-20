#include <arrow/status.h>

#include <algorithm>
#include <vector>

#include "exec/agg_hash_table.h"
#include "exec/operator_util.h"
#include "exec/operators.h"
#include "exec/spill_partitions.h"
#include "expr/eval.h"

namespace quarry {
namespace {

// Groups its whole input in a hash table and hands the groups out in batches. When the table no
// longer fits the memory budget it writes its partial states out, split by the hash of the group
// key into kSpillFanout files, empties itself and goes on. At the end every file holds all the
// partial states of the groups that hash to it, so the files are merged one at a time; a file whose
// groups still do not fit is split again on the next bits of the hash (up to kMaxSpillDepth).
class HashAggregateOperator final : public Operator {
 public:
  HashAggregateOperator(const PhysicalHashAggregate& node, std::unique_ptr<Operator> input)
      : node_(node), input_(std::move(input)) {}

  arrow::Status Open(ExecContext& context) override {
    context_ = &context;
    reservation_ =
        std::make_unique<Reservation>(*context.budget, ReservationOwner("HashAggregate", node_.id));
    std::vector<LogicalType> key_types;
    key_types.reserve(node_.groups.size());
    for (const ExprPtr& group : node_.groups) key_types.push_back(group->type);
    std::vector<AggSpec> specs;
    specs.reserve(node_.aggregates.size());
    for (const AggregateExpr& a : node_.aggregates) {
      AggSpec spec;
      spec.func = a.func;
      spec.arg_type = a.arg != nullptr ? a.arg->type : LogicalType::Int64();
      spec.result_type = a.result_type;
      specs.push_back(spec);
    }
    table_ =
        std::make_unique<AggHashTable>(std::move(key_types), std::move(specs), reservation_.get());
    // Without a spill directory a table that does not fit is an error, as before.
    table_->set_spillable(context.spill != nullptr);
    return input_->Open(context);
  }

  arrow::Result<std::optional<Batch>> Next() override {
    if (!consumed_) {
      ARROW_RETURN_NOT_OK(Consume());
      consumed_ = true;
    }
    for (;;) {
      if (table_loaded_ && next_group_ < table_->num_groups()) {
        const int64_t count =
            std::min<int64_t>(context_->batch_size, table_->num_groups() - next_group_);
        ARROW_ASSIGN_OR_RAISE(auto columns, table_->Finish(next_group_, count, context_->pool));
        next_group_ += count;
        return std::optional<Batch>(MakeBatch(std::move(columns), count));
      }
      if (!spilled_ || work_.empty()) {
        if (spilled_ && table_loaded_) table_->Clear();
        table_loaded_ = false;
        return std::optional<Batch>();
      }
      table_->Clear();
      table_loaded_ = false;
      const Work work = work_.back();
      work_.pop_back();
      ARROW_RETURN_NOT_OK(MergePartition(work));
    }
  }

  void Close() override { input_->Close(); }

  void FillStats(OperatorStats* stats) const override {
    if (reservation_ != nullptr) stats->peak_state_bytes = reservation_->peak();
    stats->spilled_bytes = spilled_bytes_;
  }

 private:
  // A file of partial states and how many times its rows have been split by hash already.
  struct Work {
    std::filesystem::path path;
    int depth = 0;
  };

  arrow::Status Consume() {
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, input_->Next());
      if (!batch.has_value()) break;
      std::vector<std::shared_ptr<arrow::Array>> keys;
      for (const ExprPtr& group : node_.groups) {
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*group, *batch, context_->pool));
        keys.push_back(std::move(column));
      }
      std::vector<std::shared_ptr<arrow::Array>> args;
      for (const AggregateExpr& a : node_.aggregates) {
        if (a.arg == nullptr) {
          args.push_back(nullptr);
          continue;
        }
        ARROW_ASSIGN_OR_RAISE(auto column, Evaluate(*a.arg, *batch, context_->pool));
        args.push_back(std::move(column));
      }
      const int64_t rows = batch->active_rows();
      const int64_t step = std::max<int64_t>(1, StateSliceRows(*context_, rows));
      for (int64_t offset = 0; offset < rows; offset += step) {
        const int64_t length = std::min(step, rows - offset);
        ARROW_RETURN_NOT_OK(
            table_->Update(Slice(keys, offset, length), Slice(args, offset, length), length));
        if (table_->over_budget()) {
          if (first_spill_ == nullptr) {
            first_spill_ = NewPartitions(0);
            spilled_ = true;
          }
          ARROW_RETURN_NOT_OK(SpillTable(first_spill_.get()));
        }
      }
    }
    if (spilled_) {
      ARROW_RETURN_NOT_OK(SpillTable(first_spill_.get()));
      ARROW_RETURN_NOT_OK(FinishPartitions(first_spill_.get(), 0));
      first_spill_.reset();
      return arrow::Status::OK();
    }
    // Without GROUP BY the answer is one row even when no row came in.
    if (node_.groups.empty()) ARROW_RETURN_NOT_OK(table_->EnsureGlobalGroup());
    table_loaded_ = true;
    return arrow::Status::OK();
  }

  static std::vector<std::shared_ptr<arrow::Array>> Slice(
      const std::vector<std::shared_ptr<arrow::Array>>& columns, int64_t offset, int64_t length) {
    std::vector<std::shared_ptr<arrow::Array>> out;
    out.reserve(columns.size());
    for (const auto& column : columns) {
      out.push_back(column == nullptr ? nullptr : column->Slice(offset, length));
    }
    return out;
  }

  std::unique_ptr<SpillPartitions> NewPartitions(int depth) {
    return std::make_unique<SpillPartitions>(*context_, SpillSchema(table_->PartialTypes()), "agg",
                                             depth);
  }

  // Writes every group's partial state to its partition and empties the table.
  arrow::Status SpillTable(SpillPartitions* partitions) {
    constexpr int64_t kChunk = 512;
    for (int64_t first = 0; first < table_->num_groups(); first += kChunk) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      const int64_t count = std::min(kChunk, table_->num_groups() - first);
      ARROW_ASSIGN_OR_RAISE(auto columns, table_->FinishPartial(first, count, context_->pool));
      std::vector<uint64_t> hashes(static_cast<size_t>(count));
      for (int64_t i = 0; i < count; ++i)
        hashes[static_cast<size_t>(i)] = table_->KeyHash(first + i);
      ARROW_RETURN_NOT_OK(
          partitions->Append(MakeBatch(std::move(columns), count), hashes, nullptr));
    }
    table_->Clear();
    return arrow::Status::OK();
  }

  // Closes the files of `partitions` and queues the non-empty ones, at `depth`, for merging.
  arrow::Status FinishPartitions(SpillPartitions* partitions, int depth) {
    const arrow::Status status = partitions->Finish();
    spilled_bytes_ += partitions->bytes_written();
    ARROW_RETURN_NOT_OK(status);
    for (int p = 0; p < kSpillFanout; ++p) {
      if (partitions->rows(p) > 0) work_.push_back({partitions->path(p), depth});
    }
    return arrow::Status::OK();
  }

  // Merges the partial states of one file into the (empty) table. When they do not fit, splits the
  // file on the next bits of the hash instead and queues the pieces.
  arrow::Status MergePartition(const Work& work) {
    ARROW_ASSIGN_OR_RAISE(auto reader, SpillReader::Open(work.path, context_->fault));
    const size_t key_count = node_.groups.size();
    std::unique_ptr<SpillPartitions> split;
    for (;;) {
      ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
      ARROW_ASSIGN_OR_RAISE(std::optional<Batch> batch, reader->Next());
      if (!batch.has_value()) break;
      const std::vector<std::shared_ptr<arrow::Array>> keys(
          batch->columns.begin(), batch->columns.begin() + static_cast<std::ptrdiff_t>(key_count));
      const std::vector<std::shared_ptr<arrow::Array>> partials(
          batch->columns.begin() + static_cast<std::ptrdiff_t>(key_count), batch->columns.end());
      if (split != nullptr) {
        ARROW_RETURN_NOT_OK(Forward(*batch, keys, split.get()));
        continue;
      }
      const int64_t step = std::max<int64_t>(1, StateSliceRows(*context_, batch->length));
      for (int64_t offset = 0; offset < batch->length; offset += step) {
        const int64_t length = std::min(step, batch->length - offset);
        if (split != nullptr) {
          ARROW_RETURN_NOT_OK(Forward(SliceBatch(*batch, offset, length),
                                      Slice(keys, offset, length), split.get()));
          continue;
        }
        ARROW_RETURN_NOT_OK(table_->MergePartial(Slice(keys, offset, length),
                                                 Slice(partials, offset, length), length));
        if (!table_->over_budget()) continue;
        if (work.depth >= kMaxSpillDepth) {
          return PartitionTooLarge(*context_, reservation_->owner(), table_->memory_bytes(),
                                   work.depth);
        }
        split = NewPartitions(work.depth + 1);
        ARROW_RETURN_NOT_OK(SpillTable(split.get()));
      }
    }
    reader.reset();
    RemoveSpillFile(work.path);
    if (split != nullptr) return FinishPartitions(split.get(), work.depth + 1);
    table_loaded_ = true;
    next_group_ = 0;
    return arrow::Status::OK();
  }

  // Routes partial states that were read from a file to the split's partitions.
  arrow::Status Forward(const Batch& batch, const std::vector<std::shared_ptr<arrow::Array>>& keys,
                        SpillPartitions* split) {
    std::vector<uint64_t> hashes;
    std::vector<uint8_t> any_null;
    ARROW_RETURN_NOT_OK(HashKeyRows(keys, batch.length, &hashes, &any_null));
    return split->Append(batch, hashes, nullptr);
  }

  const PhysicalHashAggregate& node_;
  std::unique_ptr<Operator> input_;
  ExecContext* context_ = nullptr;
  std::unique_ptr<Reservation> reservation_;
  std::unique_ptr<AggHashTable> table_;
  bool consumed_ = false;
  bool table_loaded_ = false;  // the table holds groups to hand out
  int64_t next_group_ = 0;

  bool spilled_ = false;
  std::unique_ptr<SpillPartitions> first_spill_;
  std::vector<Work> work_;
  int64_t spilled_bytes_ = 0;
};

}  // namespace

std::unique_ptr<Operator> MakeHashAggregateOperator(const PhysicalHashAggregate& node,
                                                    std::unique_ptr<Operator> input) {
  return std::make_unique<HashAggregateOperator>(node, std::move(input));
}

}  // namespace quarry
