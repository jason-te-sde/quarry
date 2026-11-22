#ifndef QUARRY_EXEC_AGG_HASH_TABLE_H_
#define QUARRY_EXEC_AGG_HASH_TABLE_H_

#include <arrow/array.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "exec/key_index.h"
#include "exec/memory_budget.h"
#include "plan/logical.h"
#include "types/logical_type.h"
#include "types/type_rules.h"

namespace quarry {

// One aggregate to compute: the function, the type of its argument (ignored for COUNT(*)) and the
// type of its result (AggregateResultType).
struct AggSpec {
  AggFunc func = AggFunc::kCountStar;
  LogicalType arg_type;
  LogicalType result_type;
};

class AggregateState;

// Groups rows by their key columns (NULLs form one group, as in GROUP BY) and keeps the state of
// every aggregate for every group. Groups are numbered in order of first appearance. The memory
// the table holds is charged to `reservation` when it is given, and the table's own storage (the
// key index and arena, and the vectors of aggregate state) is allocated from `pool`, so that the
// query's pool measures it. A growth is charged before it is allocated.
class AggHashTable {
 public:
  AggHashTable(std::vector<LogicalType> key_types, std::vector<AggSpec> aggregates,
               Reservation* reservation = nullptr, KeyHasher hasher = {},
               arrow::MemoryPool* pool = arrow::default_memory_pool());
  ~AggHashTable();  // NOLINT(performance-trivially-destructible): AggregateState is incomplete here
  AggHashTable(const AggHashTable&) = delete;
  AggHashTable& operator=(const AggHashTable&) = delete;

  // Folds `n` rows in: their key columns, and the argument column of each aggregate (null for
  // COUNT(*)). OutOfMemory when the reservation cannot grow. A spillable table that already has
  // groups when the call starts and cannot pay for the storage a new group needs before allocating
  // it stops there and returns false, with over_budget() set, and none of these rows' values folded
  // in (groups it had added by then are left with empty state): the caller spills the table, clears
  // it and calls again with the same rows. True when the rows are in.
  arrow::Result<bool> Update(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                             const std::vector<std::shared_ptr<arrow::Array>>& args, int64_t n);

  // For a table without key columns: makes sure the single group exists, so that an aggregate over
  // zero rows still yields one row.
  arrow::Status EnsureGlobalGroup();

  int64_t num_groups() const;

  // Groups [first, first + count) as columns: the key columns, then one per aggregate, of the
  // Arrow types of key_types and the aggregates' result types.
  arrow::Result<std::vector<std::shared_ptr<arrow::Array>>> Finish(int64_t first, int64_t count,
                                                                   arrow::MemoryPool* pool) const;

  int64_t memory_bytes() const;

  // Spilling. A spillable table does not fail when the reservation cannot grow: it keeps the rows
  // it was given, charges nothing for them and reports over_budget(); the caller then writes the
  // table out with FinishPartial and empties it with Clear (which also returns the charge).
  void set_spillable(bool spillable) { spillable_ = spillable; }
  bool over_budget() const { return over_budget_; }
  void Clear();
  // The Arrow types of FinishPartial's columns: the key columns, then each aggregate's partial
  // state (COUNT: the count; SUM: the sum; AVG: the sum and the count; MIN/MAX: the value).
  std::vector<std::shared_ptr<arrow::DataType>> PartialTypes() const;
  arrow::Result<std::vector<std::shared_ptr<arrow::Array>>> FinishPartial(
      int64_t first, int64_t count, arrow::MemoryPool* pool) const;
  // Folds in `n` groups' partial states, as FinishPartial wrote them: the key columns and the
  // partial columns (without the keys). Returns false in the case Update does.
  arrow::Result<bool> MergePartial(const std::vector<std::shared_ptr<arrow::Array>>& keys,
                                   const std::vector<std::shared_ptr<arrow::Array>>& partials,
                                   int64_t n);
  // The hash a spill partitions group `group` by: of the same key bytes a probe compares, so it
  // equals HashKeyRows of the group's key columns. Without key columns, the hash of the empty key.
  uint64_t KeyHash(int64_t group) const;

 private:
  arrow::Status Charge();
  arrow::Result<bool> PayForGroup(int64_t key_bytes, bool must_accept);
  void MakeStates();
  // Assigns a group to each of `n` rows, adding the ones that are new. False (nothing changed) when
  // the storage this needs cannot be paid for before it is allocated; see Update.
  arrow::Result<bool> FindGroups(const std::vector<std::shared_ptr<arrow::Array>>& keys, int64_t n,
                                 std::vector<uint32_t>* group_ids);

  std::vector<LogicalType> key_types_;
  std::vector<AggSpec> specs_;
  Reservation* reservation_;
  KeyHasher hasher_;
  arrow::MemoryPool* pool_;
  KeyIndex index_;
  std::vector<std::unique_ptr<AggregateState>> states_;
  int64_t groups_ = 0;
  int64_t states_capacity_ = 0;  // groups the state vectors hold without growing
  int64_t charged_ = 0;
  bool spillable_ = false;
  bool over_budget_ = false;
};

}  // namespace quarry

#endif  // QUARRY_EXEC_AGG_HASH_TABLE_H_
