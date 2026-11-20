#ifndef QUARRY_PARSER_AST_H_
#define QUARRY_PARSER_AST_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quarry::ast {

// The syntax tree of the SQL subset. It has no types and no names resolved: that is the binder's
// job. Every node carries the 1-based line and column of the token that starts it, so that the
// binder can report errors at the right place.

struct Position {
  int line = 1;
  int column = 1;
};

enum class BinOp : uint8_t { kAdd, kSub, kMul, kEq, kNe, kLt, kLe, kGt, kGe, kAnd, kOr };
enum class UnaryOp : uint8_t { kNegate, kNot };
enum class IntervalUnit : uint8_t { kDay, kMonth, kYear };

// "+", "-", "*", "=", "<>", "<", "<=", ">", ">=", "AND", "OR".
const char* BinOpName(BinOp op);

enum class ExprKind : uint8_t {
  kInteger,  // text: the digits
  kDecimal,  // text: the digits, with the point
  kString,   // text: the unescaped value
  kBoolean,  // boolean_value
  kNull,
  kDate,      // DATE 'text'
  kInterval,  // INTERVAL 'text' unit
  kColumn,    // qualifier (when has_qualifier) . text
  kUnary,     // unary_op children[0]
  kBinary,    // children[0] binary_op children[1]
  kBetween,   // children[0] [NOT] BETWEEN children[1] AND children[2]; negated for NOT
  kInList,    // children[0] [NOT] IN (children[1..]); negated for NOT
  kIsNull,    // children[0] IS [NOT] NULL; negated for IS NOT NULL
  kFunction,  // text(children...) or text(*) when star; text is lower-case
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::kNull;
  Position pos;
  // Height of the tree rooted here; the parser refuses a tree taller than its nesting limit.
  int depth = 1;
  std::string text;
  // An identifier written without quotes is stored lower-cased; a quoted one is stored as
  // written, and `quoted` records which it was.
  bool quoted = false;
  bool has_qualifier = false;
  std::string qualifier;
  bool qualifier_quoted = false;
  bool boolean_value = false;
  bool negated = false;
  bool star = false;
  BinOp binary_op = BinOp::kAdd;
  UnaryOp unary_op = UnaryOp::kNegate;
  int64_t interval_count = 0;
  IntervalUnit interval_unit = IntervalUnit::kDay;
  std::vector<ExprPtr> children;

  // A compact s-expression, for tests and diagnostics: (+ a (* b c)), (between x 1 2), (not (= a
  // b)).
  std::string ToString() const;
};

struct SelectItem {
  ExprPtr expr;
  Position pos;
  // The source text of the expression, used to name a column that has no alias.
  std::string source_text;
  bool has_alias = false;
  std::string alias;  // lower-cased unless alias_quoted
  bool alias_quoted = false;
};

// A table name with an optional alias, or an inner join of two FROM items. Joins are left-deep:
// `a JOIN b ON x JOIN c ON y` is ((a, b, x), c, y).
struct FromItem {
  enum class Kind : uint8_t { kTable, kJoin };
  Kind kind = Kind::kTable;
  Position pos;
  // kTable
  std::string table;
  bool table_quoted = false;
  bool has_alias = false;
  std::string alias;
  bool alias_quoted = false;
  // kJoin
  std::unique_ptr<FromItem> left;
  std::unique_ptr<FromItem> right;
  ExprPtr on;
};

enum class NullsOrder : uint8_t { kDefault, kFirst, kLast };

struct OrderItem {
  ExprPtr expr;
  bool ascending = true;
  NullsOrder nulls = NullsOrder::kDefault;
};

struct SelectStmt {
  std::vector<SelectItem> items;
  std::vector<std::unique_ptr<FromItem>> from;  // comma-separated items
  ExprPtr where;
  std::vector<ExprPtr> group_by;
  ExprPtr having;
  std::vector<OrderItem> order_by;
  std::optional<int64_t> limit;
  Position limit_pos;
};

}  // namespace quarry::ast

#endif  // QUARRY_PARSER_AST_H_
