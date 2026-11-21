#ifndef QUARRY_PARSER_PARSER_H_
#define QUARRY_PARSER_PARSER_H_

#include <memory>
#include <string>
#include <string_view>

#include "parser/ast.h"

namespace quarry {

// The nesting limit: a parenthesised expression, a chain of NOT or unary minus, and the height of
// the expression tree (so a long `1+1+1+...` as well) may not go deeper than this.
inline constexpr int kMaxNestingDepth = 256;

// The parser has no Arrow, so it cannot return an arrow::Result; the binder turns `error` into an
// arrow::Status (see ParseAndBind in binder/binder.h).
struct ParseResult {
  std::unique_ptr<ast::SelectStmt> stmt;  // set when the parse succeeded
  std::string error;                      // "syntax error at L:C: ..." when it did not

  bool ok() const { return stmt != nullptr; }
};

// Parses one SELECT statement, optionally ended by ';'. Never throws and never recurses deeper than
// kMaxNestingDepth, whatever the input: too deep a nesting is a "syntax error ... nesting too
// deep".
ParseResult Parse(std::string_view sql);

}  // namespace quarry

#endif  // QUARRY_PARSER_PARSER_H_
