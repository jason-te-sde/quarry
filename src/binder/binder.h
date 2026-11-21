#ifndef QUARRY_BINDER_BINDER_H_
#define QUARRY_BINDER_BINDER_H_

#include <arrow/result.h>

#include <string_view>

#include "binder/catalog.h"
#include "parser/ast.h"
#include "plan/logical.h"

namespace quarry {

// Resolves names against the catalog, infers types (inserting the implicit casts of expr/), and
// returns the logical plan: scans, a cross product or joins for FROM, a Filter for WHERE, an
// Aggregate for GROUP BY and aggregate calls, a Filter for HAVING, a Project for the select list,
// a Sort and a Limit. Errors are arrow::Status::Invalid with the message prefix
// "binder error at <line>:<column>: ".
//
// The select list's output column names are the alias, else the column name, else the expression
// as written.
arrow::Result<LogicalPlan> Bind(const ast::SelectStmt& stmt, const Catalog& catalog);

// Parse then Bind. A parse failure is Invalid with its "syntax error at <line>:<column>: " text.
arrow::Result<LogicalPlan> ParseAndBind(std::string_view sql, const Catalog& catalog);

}  // namespace quarry

#endif  // QUARRY_BINDER_BINDER_H_
