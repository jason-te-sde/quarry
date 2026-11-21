#ifndef QUARRY_EXEC_BUILD_OPERATORS_H_
#define QUARRY_EXEC_BUILD_OPERATORS_H_

#include <arrow/result.h>

#include <memory>
#include <vector>

#include "exec/exec_context.h"
#include "exec/operator.h"
#include "exec/profile.h"
#include "plan/physical.h"

namespace quarry {

// Builds the operator tree of a physical plan. Every operator is wrapped so that it counts its
// rows and batches, measures its time and checks for cancellation and the deadline once per Next
// call. The plan must outlive the operators.
arrow::Result<std::unique_ptr<Operator>> BuildOperators(const PhysicalNode& root,
                                                        ExecContext& context);

// The profile of a tree made by BuildOperators, one entry per operator, ordered by plan node id.
// Call it after the query has run.
std::vector<OperatorStats> CollectOperatorStats(const Operator& root);

}  // namespace quarry

#endif  // QUARRY_EXEC_BUILD_OPERATORS_H_
