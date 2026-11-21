#ifndef QUARRY_EXEC_OPERATORS_H_
#define QUARRY_EXEC_OPERATORS_H_

// The operators, built one at a time from a descriptor and the operators that feed them.
// BuildOperators (build_operators.h) does that for a whole plan; tests use these directly with
// hand-made inputs. The descriptor's own child pointers are not used: the inputs are the arguments.
// The descriptor must outlive the operator.

#include <memory>

#include "exec/operator.h"
#include "plan/physical.h"

namespace quarry {

std::unique_ptr<Operator> MakeFilterOperator(const PhysicalFilter& node,
                                             std::unique_ptr<Operator> input);
std::unique_ptr<Operator> MakeProjectOperator(const PhysicalProject& node,
                                              std::unique_ptr<Operator> input);
// Inner equi-join. The build input is read completely on the first Next; the probe input streams.
std::unique_ptr<Operator> MakeHashJoinOperator(const PhysicalHashJoin& node,
                                               std::unique_ptr<Operator> probe,
                                               std::unique_ptr<Operator> build);
std::unique_ptr<Operator> MakeHashAggregateOperator(const PhysicalHashAggregate& node,
                                                    std::unique_ptr<Operator> input);
std::unique_ptr<Operator> MakeSortOperator(const PhysicalSort& node,
                                           std::unique_ptr<Operator> input);
std::unique_ptr<Operator> MakeTopNOperator(const PhysicalTopN& node,
                                           std::unique_ptr<Operator> input);
std::unique_ptr<Operator> MakeLimitOperator(const PhysicalLimit& node,
                                            std::unique_ptr<Operator> input);

}  // namespace quarry

#endif  // QUARRY_EXEC_OPERATORS_H_
