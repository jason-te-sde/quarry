#ifndef QUARRY_PLANNER_PHYSICAL_PLANNER_H_
#define QUARRY_PLANNER_PHYSICAL_PLANNER_H_

#include <arrow/result.h>

#include "plan/logical.h"
#include "plan/physical.h"
#include "planner/rules.h"

namespace quarry {

// Turns an optimized logical plan into descriptors, numbering them in pre-order. A Join's right
// input is the build side (pick_build_side decides which input that is). A Limit over a Sort
// becomes one TopN, also when a Project that only trims hidden sort columns sits between them (the
// Project then goes on top of the TopN). Unless prune_row_groups is disabled, each scan lists its
// prunable conjuncts. An unknown rule name is Invalid.
arrow::Result<PhysicalPlan> PlanPhysical(const LogicalPlan& plan, const PlannerOptions& options);

}  // namespace quarry

#endif  // QUARRY_PLANNER_PHYSICAL_PLANNER_H_
