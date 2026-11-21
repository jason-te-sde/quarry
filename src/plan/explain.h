#ifndef QUARRY_PLAN_EXPLAIN_H_
#define QUARRY_PLAN_EXPLAIN_H_

#include <string>

#include "plan/physical.h"

namespace quarry {

// One node per line, two spaces of indent per depth, every line ended by '\n':
//   <NodeName> #<id> [<params>] est=<estimated rows, rounded to an integer>
// The names are Scan, Filter, Project, HashJoin, HashAggregate, Sort, TopN and Limit. A join prints
// its probe input first, then its build input. File paths are not printed.
std::string Explain(const PhysicalNode& node);

}  // namespace quarry

#endif  // QUARRY_PLAN_EXPLAIN_H_
