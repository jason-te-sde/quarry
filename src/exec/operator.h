#ifndef QUARRY_EXEC_OPERATOR_H_
#define QUARRY_EXEC_OPERATOR_H_

#include <arrow/result.h>
#include <arrow/status.h>

#include <optional>
#include <vector>

#include "exec/exec_context.h"
#include "exec/profile.h"
#include "vector/batch.h"

namespace quarry {

// A pull-based operator: the consumer calls Next until it returns std::nullopt, which is the end of
// the stream. An operator never returns a batch with no live rows (a filter may drop a whole batch;
// it then pulls the next one). A batch may carry a selection vector. Open is called once before
// the first Next, Close once after the last (also after an error). An operator's Reservation shares
// ownership of the budget's counters, so operators may be destroyed after the budget.
class Operator {
 public:
  virtual ~Operator() = default;

  virtual arrow::Status Open(ExecContext& context) = 0;
  virtual arrow::Result<std::optional<Batch>> Next() = 0;
  virtual void Close() = 0;

  // Adds what this operator measured itself to `stats` (peak state, spilled bytes, scan buffer).
  virtual void FillStats(OperatorStats* stats) const { (void)stats; }
  // Appends the profile of this operator and of its inputs. Only the profiling wrapper does.
  virtual void CollectProfile(std::vector<OperatorStats>* out) const { (void)out; }
};

}  // namespace quarry

#endif  // QUARRY_EXEC_OPERATOR_H_
