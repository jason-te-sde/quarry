#ifndef QUARRY_PLANNER_JOIN_GRAPH_H_
#define QUARRY_PLANNER_JOIN_GRAPH_H_

#include <arrow/result.h>

#include <vector>

#include "expr/bound_expr.h"
#include "plan/logical.h"
#include "planner/plan_util.h"

namespace quarry {

struct JoinTree {
  LogicalPlan plan;
  // Old column index (relations side by side, in the order they were given) -> index in `plan`.
  ColumnMap map;
  // The conjuncts that are not equalities between two relations, redirected to `plan`'s output.
  std::vector<ExprPtr> leftover;
};

// Joins the relations of a comma join into one left-deep tree of LogicalJoins.
//
// `conjuncts` are predicates over the relations' concatenated output. Each one of the form
// `x = y`, where x uses columns of one relation and y of another, is an edge of the join graph and
// becomes a key pair of the join that brings the second of the two relations in; the others come
// back in `leftover`. A relation not connected to the first one through edges is Invalid:
// "planner error at 1:1: requires a cross product between <first> and <unreached>".
//
// `greedy` picks the order: start from the relation with the smallest estimate, then keep adding
// the connected relation that gives the smallest estimated join result (ties: the earlier one in
// FROM order). Otherwise the order is FROM order, where a relation that is not yet connected waits
// for the first later one that is.
arrow::Result<JoinTree> BuildJoinTree(std::vector<LogicalPlan> relations,
                                      std::vector<ExprPtr> conjuncts, bool greedy);

}  // namespace quarry

#endif  // QUARRY_PLANNER_JOIN_GRAPH_H_
