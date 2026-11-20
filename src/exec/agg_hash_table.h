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
// the table holds is charged to `reservation` when it is given.
class AggHashTable {
 public:
  AggHashTable(std::vector<LogicalType> key_types, std::vector<AggSpec> aggregates,
               Reservation* reservation = nullptr, KeyHasher hasher = {});
  ~AggHashTable();  // NOLINT(performance-trivially-destructible): AggregateState is incomplete here
  AggHashTable(const AggHashTable&) = delete;
  AggHashTable& operator=(const AggHashTable&) = delete;

  // Folds `n` rows in: their key columns, and the argument column of each aggregate (null for
  // COUNT(*)). OutOfMemory when the reservation cannot grow.
  arrow::Status Update(const std::vector<std::shared_ptr<arrow::Array>>& keys,
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

 private:
  arrow::Status Charge();

  std::vector<LogicalType> key_types_;
  std::vector<AggSpec> specs_;
  Reservation* reservation_;
  KeyIndex index_;
  std::vector<std::unique_ptr<AggregateState>> states_;
  int64_t groups_ = 0;
  int64_t charged_ = 0;
};

}  // namespace quarry

#endif  // QUARRY_EXEC_AGG_HASH_TABLE_H_
