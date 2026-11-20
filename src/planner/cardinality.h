#ifndef QUARRY_PLANNER_CARDINALITY_H_
#define QUARRY_PLANNER_CARDINALITY_H_

#include <vector>

#include "expr/bound_expr.h"
#include "plan/logical.h"

namespace quarry {

// The fraction of rows a predicate is assumed to keep. The constants are fixed, not measured:
//   =  0.1       <>  0.9       <, <=, >, >=, BETWEEN  0.3
//   IN (n values)  min(0.5, 0.1 * n)       IS NULL  0.1       IS NOT NULL  0.9
//   a OR b  s1 + s2 - s1 * s2     a AND b  s1 * s2     NOT a  1 - s
// Anything else keeps everything (1.0); a literal FALSE or NULL keeps nothing.
double EstimateSelectivity(const BoundExpr& predicate);

// The estimated size of a join result, given the sizes of its inputs and, for each key pair, the
// row count of the smaller of the two base tables the pair's columns come from (the PK-FK
// assumption: the smaller table holds the key, every row of the larger finds one match).
//   no key pair:  l * r
//   otherwise:    l * r / key_base_rows[0], times 0.1 for every further pair
double EstimateJoinRows(double left_rows, double right_rows,
                        const std::vector<double>& key_base_rows);

// The row count of a key expression's base table: the table of the scan the key's first column
// comes from, followed through filters, sorts, limits, joins and plain column projections. When the
// column does not come from a scan, the estimate of `side` itself.
double KeyBaseRows(const LogicalNode& side, const BoundExpr& key);

// The estimated number of rows `node` produces:
//   Scan       row_count * the product of the selectivities of its filters
//   Filter     child * selectivity       Project, Sort  child       Limit  min(child, n)
//   Join       EstimateJoinRows over its key pairs, times the selectivity of its residual
//   Cross      the product of its children
//   Aggregate  1 without groups, otherwise a tenth of the child (at least 1); never used to
//              choose between plans
double EstimateRows(const LogicalNode& node);

}  // namespace quarry

#endif  // QUARRY_PLANNER_CARDINALITY_H_
