#include "binder/binder.h"

#include <arrow/scalar.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "expr/bound_expr.h"
#include "parser/parser.h"
#include "types/date.h"
#include "types/logical_type.h"
#include "types/type_rules.h"

namespace quarry {
namespace {

using AstKind = ast::ExprKind;
using ExprResult = arrow::Result<ExprPtr>;

arrow::Status Err(const ast::Position& pos, const std::string& message) {
  return arrow::Status::Invalid("binder error at ", pos.line, ":", pos.column, ": ", message);
}

// Re-reports an error of a factory (which knows no position) at `pos`.
template <typename T>
arrow::Result<T> At(const ast::Position& pos, arrow::Result<T> result) {
  if (result.ok()) return result;
  return Err(pos, result.status().message());
}

// ---- scopes: which names a FROM clause makes visible ----

struct ScopeColumn {
  std::string name;
  int index = -1;  // position in the scope's row; -1 when the column cannot be used
  LogicalType type;
  std::string reason;  // why it cannot be used
};

struct ScopeTable {
  std::string alias;  // the name that qualifies its columns
  ast::Position pos;
  std::vector<ScopeColumn> columns;
};

struct Scope {
  std::vector<ScopeTable> tables;
  int width = 0;  // columns of the row the scope describes
};

// Appends `right`'s tables after `left`'s, shifting their column indices: the row of the combined
// scope is left's row followed by right's.
arrow::Status AppendScope(Scope* left, Scope right) {
  for (ScopeTable& table : right.tables) {
    for (const ScopeTable& existing : left->tables) {
      if (existing.alias == table.alias) {
        return Err(table.pos, "table name " + table.alias +
                                  " is used more than once; give each use a distinct alias");
      }
    }
    for (ScopeColumn& column : table.columns) {
      if (column.index >= 0) column.index += left->width;
    }
    left->tables.push_back(std::move(table));
  }
  left->width += right.width;
  return arrow::Status::OK();
}

// ---- helpers on bound and unbound expressions ----

bool IsAggregateName(const std::string& name) {
  return name == "sum" || name == "avg" || name == "count" || name == "min" || name == "max";
}

bool HasAggregate(const ast::Expr& e) {
  if (e.kind == AstKind::kFunction && IsAggregateName(e.text)) return true;
  return std::any_of(e.children.begin(), e.children.end(),
                     [](const ast::ExprPtr& c) { return HasAggregate(*c); });
}

// The smallest and largest column index an expression reads; min > max when it reads none.
void ColumnRange(const BoundExpr& e, int* lo, int* hi) {
  if (e.kind == ExprKind::kColumn) {
    *lo = std::min(*lo, e.column_index);
    *hi = std::max(*hi, e.column_index);
  }
  for (const ExprPtr& child : e.children) ColumnRange(*child, lo, hi);
}

void ShiftColumns(BoundExpr* e, int delta) {
  if (e->kind == ExprKind::kColumn) e->column_index += delta;
  for (ExprPtr& child : e->children) ShiftColumns(child.get(), delta);
}

void FlattenAnd(const ast::Expr& e, std::vector<const ast::Expr*>* out) {
  if (e.kind == AstKind::kBinary && e.binary_op == ast::BinOp::kAnd) {
    FlattenAnd(*e.children[0], out);
    FlattenAnd(*e.children[1], out);
  } else {
    out->push_back(&e);
  }
}

BinaryOp ToBinaryOp(ast::BinOp op) {
  switch (op) {
    case ast::BinOp::kAdd:
      return BinaryOp::kAdd;
    case ast::BinOp::kSub:
      return BinaryOp::kSub;
    case ast::BinOp::kMul:
      return BinaryOp::kMul;
    case ast::BinOp::kEq:
      return BinaryOp::kEq;
    case ast::BinOp::kNe:
      return BinaryOp::kNe;
    case ast::BinOp::kLt:
      return BinaryOp::kLt;
    case ast::BinOp::kLe:
      return BinaryOp::kLe;
    case ast::BinOp::kGt:
      return BinaryOp::kGt;
    case ast::BinOp::kGe:
      return BinaryOp::kGe;
    case ast::BinOp::kAnd:
      return BinaryOp::kAnd;
    case ast::BinOp::kOr:
      return BinaryOp::kOr;
  }
  return BinaryOp::kAdd;
}

IntervalUnit ToIntervalUnit(ast::IntervalUnit unit) {
  switch (unit) {
    case ast::IntervalUnit::kDay:
      return IntervalUnit::kDay;
    case ast::IntervalUnit::kMonth:
      return IntervalUnit::kMonth;
    case ast::IntervalUnit::kYear:
      return IntervalUnit::kYear;
  }
  return IntervalUnit::kDay;
}

// -x for a literal integer or decimal, so that `-5` is the literal -5; null for anything else.
std::shared_ptr<arrow::Scalar> NegatedLiteral(const arrow::Scalar& s) {
  if (!s.is_valid) return nullptr;
  switch (s.type->id()) {
    case arrow::Type::INT32: {
      const int32_t v = static_cast<const arrow::Int32Scalar&>(s).value;
      if (v == std::numeric_limits<int32_t>::min()) return nullptr;
      return std::make_shared<arrow::Int32Scalar>(-v);
    }
    case arrow::Type::INT64: {
      const int64_t v = static_cast<const arrow::Int64Scalar&>(s).value;
      if (v == std::numeric_limits<int64_t>::min()) return nullptr;
      return std::make_shared<arrow::Int64Scalar>(-v);
    }
    case arrow::Type::DECIMAL64: {
      arrow::Decimal64 v = static_cast<const arrow::Decimal64Scalar&>(s).value;
      v.Negate();
      return std::make_shared<arrow::Decimal64Scalar>(v, s.type);
    }
    case arrow::Type::DECIMAL128: {
      arrow::Decimal128 v = static_cast<const arrow::Decimal128Scalar&>(s).value;
      v.Negate();
      return std::make_shared<arrow::Decimal128Scalar>(v, s.type);
    }
    default:
      return nullptr;
  }
}

// ---- the binder ----

enum class Clause : uint8_t {
  kWhere,
  kOn,
  kGroupBy,
  kSelect,
  kHaving,
  kOrderBy,
  kAggregateArgument
};

const char* ClauseName(Clause c) {
  switch (c) {
    case Clause::kWhere:
      return "WHERE";
    case Clause::kOn:
      return "JOIN ... ON";
    case Clause::kGroupBy:
      return "GROUP BY";
    case Clause::kSelect:
      return "the select list";
    case Clause::kHaving:
      return "HAVING";
    case Clause::kOrderBy:
      return "ORDER BY";
    case Clause::kAggregateArgument:
      return "an aggregate argument";
  }
  return "";
}

// One aggregate call found in the select list, HAVING or ORDER BY.
struct AggregateCall {
  AggregateExpr expr;
  std::string key;  // "SUM(#3)": identical calls are computed once
};

// How an expression is bound.
//   Row: against the rows of FROM (WHERE, ON, GROUP BY, aggregate arguments, and every clause of a
//        query without aggregation).
//   Post: against the output of the Aggregate: a sub-expression equal to a GROUP BY expression is
//        that group column, an aggregate call is an aggregate column, and any other column is an
//        error. The Row binding of the same sub-expression is how equality is decided.
struct ExprContext {
  const Scope* scope = nullptr;
  Clause clause = Clause::kSelect;
  bool post_aggregate = false;
  const std::vector<std::string>* group_keys = nullptr;
  const std::vector<LogicalType>* group_types = nullptr;
  std::vector<AggregateCall>* aggregates = nullptr;
};

class Binder {
 public:
  explicit Binder(const Catalog& catalog) : catalog_(catalog) {}

  arrow::Result<LogicalPlan> Run(const ast::SelectStmt& stmt);

 private:
  struct BoundFrom {
    LogicalPlan plan;
    Scope scope;
  };

  arrow::Result<BoundFrom> BindFromItem(const ast::FromItem& item);
  arrow::Result<BoundFrom> BindTable(const ast::FromItem& item);
  arrow::Result<BoundFrom> BindJoin(const ast::FromItem& item);

  ExprResult BindExpr(const ast::Expr& e, const ExprContext& ctx);
  ExprResult BindBooleanClause(const ast::Expr& e, const ExprContext& ctx, const char* what);
  ExprResult BindColumn(const ast::Expr& e, const Scope& scope);
  ExprResult BindLiteral(const ast::Expr& e);
  ExprResult BindBinary(const ast::Expr& e, const ExprContext& ctx);
  ExprResult BindFunction(const ast::Expr& e, const ExprContext& ctx);
  ExprResult BindNullLike(const ast::Expr& e, const ExprContext& ctx, const LogicalType& type);
  ExprResult BindOperand(const ast::Expr& e, const ExprContext& ctx, const LogicalType* null_type);
  ExprResult BindPostAggregate(const ast::Expr& e, const ExprContext& ctx);

  const Catalog& catalog_;
};

// ---- FROM ----

arrow::Result<Binder::BoundFrom> Binder::BindTable(const ast::FromItem& item) {
  const TableInfo* table = catalog_.Find(item.table);
  if (table == nullptr) return Err(item.pos, "unknown table " + item.table);

  auto scan = std::make_unique<LogicalScan>();
  scan->table = table;
  scan->table_name = table->name;
  scan->line = item.pos.line;
  scan->column = item.pos.column;
  ScopeTable scope_table;
  scope_table.alias = item.has_alias ? item.alias : item.table;
  scope_table.pos = item.pos;
  for (size_t i = 0; i < table->columns.size(); ++i) {
    const ColumnInfo& column = table->columns[i];
    scan->table_column_names.push_back(column.name);
    scan->table_column_types.push_back(column.type);
    ScopeColumn sc;
    sc.name = column.name;
    if (column.type.has_value()) {
      sc.index = static_cast<int>(scan->columns.size());
      sc.type = *column.type;
      scan->columns.push_back(static_cast<int>(i));
    } else {
      sc.reason = column.unsupported_reason;
    }
    scope_table.columns.push_back(std::move(sc));
  }
  BoundFrom out;
  out.scope.width = static_cast<int>(scan->columns.size());
  out.scope.tables.push_back(std::move(scope_table));
  out.plan = std::move(scan);
  return out;
}

arrow::Result<Binder::BoundFrom> Binder::BindJoin(const ast::FromItem& item) {
  ARROW_ASSIGN_OR_RAISE(BoundFrom left, BindFromItem(*item.left));
  ARROW_ASSIGN_OR_RAISE(BoundFrom right, BindFromItem(*item.right));
  const int left_width = left.scope.width;

  BoundFrom out;
  out.scope = std::move(left.scope);
  ARROW_RETURN_NOT_OK(AppendScope(&out.scope, std::move(right.scope)));

  auto join = std::make_unique<LogicalJoin>();
  join->left = std::move(left.plan);
  join->right = std::move(right.plan);

  ExprContext ctx;
  ctx.scope = &out.scope;
  ctx.clause = Clause::kOn;
  std::vector<const ast::Expr*> conjuncts;
  FlattenAnd(*item.on, &conjuncts);
  std::vector<ExprPtr> rest;
  for (const ast::Expr* c : conjuncts) {
    const bool is_equality = c->kind == AstKind::kBinary && c->binary_op == ast::BinOp::kEq &&
                             c->children[0]->kind != AstKind::kNull &&
                             c->children[1]->kind != AstKind::kNull;
    if (is_equality) {
      ARROW_ASSIGN_OR_RAISE(ExprPtr l, BindExpr(*c->children[0], ctx));
      ARROW_ASSIGN_OR_RAISE(ExprPtr r, BindExpr(*c->children[1], ctx));
      int l_lo = std::numeric_limits<int>::max(), l_hi = -1;
      int r_lo = std::numeric_limits<int>::max(), r_hi = -1;
      ColumnRange(*l, &l_lo, &l_hi);
      ColumnRange(*r, &r_lo, &r_hi);
      const bool l_left = l_hi >= 0 && l_hi < left_width;
      const bool l_right = l_hi >= 0 && l_lo >= left_width;
      const bool r_left = r_hi >= 0 && r_hi < left_width;
      const bool r_right = r_hi >= 0 && r_lo >= left_width;
      if ((l_left && r_right) || (l_right && r_left)) {
        if (l_right) std::swap(l, r);
        // Comparing builds the casts that bring both keys to one type; the keys are its operands.
        ARROW_ASSIGN_OR_RAISE(ExprPtr eq,
                              At(c->pos, MakeBinary(BinaryOp::kEq, std::move(l), std::move(r))));
        ShiftColumns(eq->children[1].get(), -left_width);
        join->left_keys.push_back(std::move(eq->children[0]));
        join->right_keys.push_back(std::move(eq->children[1]));
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(ExprPtr eq,
                            At(c->pos, MakeBinary(BinaryOp::kEq, std::move(l), std::move(r))));
      rest.push_back(std::move(eq));
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(ExprPtr bound, BindBooleanClause(*c, ctx, "ON"));
    rest.push_back(std::move(bound));
  }
  for (ExprPtr& e : rest) {
    if (join->residual == nullptr) {
      join->residual = std::move(e);
    } else {
      ARROW_ASSIGN_OR_RAISE(
          join->residual,
          At(item.pos, MakeBinary(BinaryOp::kAnd, std::move(join->residual), std::move(e))));
    }
  }
  out.plan = std::move(join);
  return out;
}

arrow::Result<Binder::BoundFrom> Binder::BindFromItem(const ast::FromItem& item) {
  return item.kind == ast::FromItem::Kind::kTable ? BindTable(item) : BindJoin(item);
}

// ---- expressions ----

ExprResult Binder::BindBooleanClause(const ast::Expr& e, const ExprContext& ctx, const char* what) {
  ARROW_ASSIGN_OR_RAISE(ExprPtr bound, BindExpr(e, ctx));
  if (bound->type.id != TypeId::kBoolean) {
    return Err(e.pos, std::string(what) + " must be a BOOLEAN expression, found " +
                          bound->type.ToString());
  }
  return bound;
}

ExprResult Binder::BindColumn(const ast::Expr& e, const Scope& scope) {
  struct Match {
    const ScopeTable* table;
    const ScopeColumn* column;
  };
  std::vector<Match> matches;
  if (e.has_qualifier) {
    const ScopeTable* table = nullptr;
    for (const ScopeTable& t : scope.tables) {
      if (t.alias == e.qualifier) table = &t;
    }
    if (table == nullptr) return Err(e.pos, "unknown table or alias " + e.qualifier);
    for (const ScopeColumn& c : table->columns) {
      if (c.name == e.text) matches.push_back({table, &c});
    }
    if (matches.empty()) return Err(e.pos, "unknown column " + e.qualifier + "." + e.text);
  } else {
    for (const ScopeTable& t : scope.tables) {
      for (const ScopeColumn& c : t.columns) {
        if (c.name == e.text) matches.push_back({&t, &c});
      }
    }
    if (matches.empty()) return Err(e.pos, "unknown column " + e.text);
  }
  if (matches.size() > 1) {
    std::string candidates;
    for (const Match& m : matches) {
      if (!candidates.empty()) candidates += ", ";
      candidates += m.table->alias + "." + m.column->name;
    }
    return Err(e.pos, "column reference " + e.text + " is ambiguous: it could be " + candidates);
  }
  const ScopeColumn& column = *matches[0].column;
  if (column.index < 0) return Err(e.pos, column.reason);
  return MakeColumn(column.index, column.type);
}

ExprResult Binder::BindLiteral(const ast::Expr& e) {
  switch (e.kind) {
    case AstKind::kInteger: {
      int64_t value = 0;
      for (const char c : e.text) {
        const int digit = c - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
          return Err(e.pos, "integer literal " + e.text + " is out of range");
        }
        value = value * 10 + digit;
      }
      if (value <= std::numeric_limits<int32_t>::max()) {
        return At(e.pos,
                  MakeLiteral(std::make_shared<arrow::Int32Scalar>(static_cast<int32_t>(value))));
      }
      return At(e.pos, MakeLiteral(std::make_shared<arrow::Int64Scalar>(value)));
    }
    case AstKind::kDecimal:
      return At(e.pos, MakeDecimalLiteral(e.text));
    case AstKind::kString:
      return At(e.pos, MakeLiteral(std::make_shared<arrow::StringScalar>(e.text)));
    case AstKind::kBoolean:
      return At(e.pos, MakeLiteral(std::make_shared<arrow::BooleanScalar>(e.boolean_value)));
    case AstKind::kNull:
      // A bare NULL is an INTEGER, like DuckDB; next to another operand it takes that type.
      return At(e.pos, MakeLiteral(arrow::MakeNullScalar(arrow::int32())));
    case AstKind::kDate: {
      ARROW_ASSIGN_OR_RAISE(const int32_t days, At(e.pos, ParseDate(e.text)));
      return At(e.pos, MakeLiteral(std::make_shared<arrow::Date32Scalar>(days)));
    }
    default:
      return Err(e.pos, "not a literal");
  }
}

ExprResult Binder::BindNullLike(const ast::Expr& e, const ExprContext&, const LogicalType& type) {
  return At(e.pos, MakeLiteral(arrow::MakeNullScalar(type.ToArrow())));
}

// Binds an operand; a NULL literal takes `null_type` when one is known.
ExprResult Binder::BindOperand(const ast::Expr& e, const ExprContext& ctx,
                               const LogicalType* null_type) {
  if (e.kind == AstKind::kNull && null_type != nullptr) return BindNullLike(e, ctx, *null_type);
  return BindExpr(e, ctx);
}

ExprResult Binder::BindBinary(const ast::Expr& e, const ExprContext& ctx) {
  const ast::Expr& lhs = *e.children[0];
  const ast::Expr& rhs = *e.children[1];

  // DATE +/- INTERVAL, folded here when the date is a constant.
  if (rhs.kind == AstKind::kInterval) {
    if (e.binary_op != ast::BinOp::kAdd && e.binary_op != ast::BinOp::kSub) {
      return Err(rhs.pos, "INTERVAL can only be added to or subtracted from a DATE");
    }
    ARROW_ASSIGN_OR_RAISE(ExprPtr date, BindExpr(lhs, ctx));
    const bool constant_date = date->kind == ExprKind::kLiteral && date->type.id == TypeId::kDate &&
                               date->literal != nullptr && date->literal->is_valid;
    if (!constant_date) {
      return Err(e.pos,
                 "INTERVAL arithmetic needs a constant DATE on the left, as in DATE '1998-12-01' - "
                 "INTERVAL '90' DAY");
    }
    const int32_t days = static_cast<const arrow::Date32Scalar&>(*date->literal).value;
    const int64_t n = e.binary_op == ast::BinOp::kSub ? -rhs.interval_count : rhs.interval_count;
    ARROW_ASSIGN_OR_RAISE(const int32_t folded,
                          At(e.pos, AddInterval(days, n, ToIntervalUnit(rhs.interval_unit))));
    return At(e.pos, MakeLiteral(std::make_shared<arrow::Date32Scalar>(folded)));
  }

  ExprPtr l;
  ExprPtr r;
  if (lhs.kind == AstKind::kNull && rhs.kind != AstKind::kNull) {
    ARROW_ASSIGN_OR_RAISE(r, BindExpr(rhs, ctx));
    ARROW_ASSIGN_OR_RAISE(l, BindNullLike(lhs, ctx, r->type));
  } else {
    ARROW_ASSIGN_OR_RAISE(l, BindExpr(lhs, ctx));
    ARROW_ASSIGN_OR_RAISE(r, BindOperand(rhs, ctx, &l->type));
  }
  return At(e.pos, MakeBinary(ToBinaryOp(e.binary_op), std::move(l), std::move(r)));
}

ExprResult Binder::BindFunction(const ast::Expr& e, const ExprContext& ctx) {
  if (!IsAggregateName(e.text)) return Err(e.pos, "unknown function " + e.text);
  if (!ctx.post_aggregate) {
    if (ctx.clause == Clause::kAggregateArgument) {
      return Err(e.pos, "aggregate function calls cannot be nested");
    }
    return Err(e.pos,
               std::string("aggregate functions are not allowed in ") + ClauseName(ctx.clause));
  }

  AggregateCall call;
  AggFunc func = AggFunc::kCount;
  if (e.text == "sum") func = AggFunc::kSum;
  if (e.text == "avg") func = AggFunc::kAvg;
  if (e.text == "min") func = AggFunc::kMin;
  if (e.text == "max") func = AggFunc::kMax;

  LogicalType arg_type = LogicalType::Int32();
  if (e.star) {
    if (e.text != "count")
      return Err(e.pos, e.text + "(*) is not valid: only COUNT(*) takes a star");
    func = AggFunc::kCountStar;
  } else {
    if (e.children.size() != 1) {
      return Err(e.pos, e.text + " takes exactly one argument");
    }
    ExprContext arg_ctx;
    arg_ctx.scope = ctx.scope;
    arg_ctx.clause = Clause::kAggregateArgument;
    ARROW_ASSIGN_OR_RAISE(call.expr.arg, BindExpr(*e.children[0], arg_ctx));
    arg_type = call.expr.arg->type;
  }
  call.expr.func = func;
  ARROW_ASSIGN_OR_RAISE(call.expr.result_type, At(e.pos, AggregateResultType(func, arg_type)));
  call.key = std::string(AggFuncName(func)) +
             (call.expr.arg == nullptr ? "" : "(" + call.expr.arg->ToString() + ")");

  const size_t groups = ctx.group_keys->size();
  const LogicalType type = call.expr.result_type;
  for (size_t i = 0; i < ctx.aggregates->size(); ++i) {
    if ((*ctx.aggregates)[i].key == call.key) {
      return MakeColumn(static_cast<int>(groups + i), type);
    }
  }
  ctx.aggregates->push_back(std::move(call));
  return MakeColumn(static_cast<int>(groups + ctx.aggregates->size() - 1), type);
}

// The Post-mode step shared by every node: an expression equal to a GROUP BY expression becomes the
// group column. Returns null when there is no such match.
ExprResult Binder::BindPostAggregate(const ast::Expr& e, const ExprContext& ctx) {
  if (HasAggregate(e)) return ExprPtr();
  ExprContext row = ctx;
  row.post_aggregate = false;
  auto bound = BindExpr(e, row);
  if (!bound.ok()) return ExprPtr();
  const std::string key = (*bound)->ToString();
  for (size_t i = 0; i < ctx.group_keys->size(); ++i) {
    if ((*ctx.group_keys)[i] == key) {
      return MakeColumn(static_cast<int>(i), (*ctx.group_types)[i]);
    }
  }
  return ExprPtr();
}

ExprResult Binder::BindExpr(const ast::Expr& e, const ExprContext& ctx) {
  if (ctx.post_aggregate && e.kind != AstKind::kFunction) {
    ARROW_ASSIGN_OR_RAISE(ExprPtr grouped, BindPostAggregate(e, ctx));
    if (grouped != nullptr) return grouped;
  }
  switch (e.kind) {
    case AstKind::kInteger:
    case AstKind::kDecimal:
    case AstKind::kString:
    case AstKind::kBoolean:
    case AstKind::kNull:
    case AstKind::kDate:
      return BindLiteral(e);
    case AstKind::kInterval:
      return Err(e.pos,
                 "INTERVAL is only supported as the right operand of + or - after a constant DATE");
    case AstKind::kColumn: {
      // Unknown and ambiguous names are reported before a GROUP BY mismatch.
      ARROW_ASSIGN_OR_RAISE(ExprPtr column, BindColumn(e, *ctx.scope));
      if (ctx.post_aggregate) {
        const std::string name = e.has_qualifier ? e.qualifier + "." + e.text : e.text;
        return Err(e.pos, "column " + name +
                              " must appear in the GROUP BY clause or be used in an aggregate "
                              "function");
      }
      return column;
    }
    case AstKind::kUnary: {
      ARROW_ASSIGN_OR_RAISE(ExprPtr child, BindExpr(*e.children[0], ctx));
      if (e.unary_op == ast::UnaryOp::kNot) return At(e.pos, MakeNot(std::move(child)));
      if (child->kind == ExprKind::kLiteral) {
        if (auto negated = NegatedLiteral(*child->literal)) {
          return At(e.pos, MakeLiteral(std::move(negated)));
        }
      }
      return At(e.pos, MakeNegate(std::move(child)));
    }
    case AstKind::kBinary:
      return BindBinary(e, ctx);
    case AstKind::kBetween: {
      ARROW_ASSIGN_OR_RAISE(ExprPtr x, BindExpr(*e.children[0], ctx));
      const LogicalType type = x->type;
      ARROW_ASSIGN_OR_RAISE(ExprPtr lo, BindOperand(*e.children[1], ctx, &type));
      ARROW_ASSIGN_OR_RAISE(ExprPtr hi, BindOperand(*e.children[2], ctx, &type));
      ARROW_ASSIGN_OR_RAISE(ExprPtr between,
                            At(e.pos, MakeBetween(std::move(x), std::move(lo), std::move(hi))));
      if (!e.negated) return between;
      return At(e.pos, MakeNot(std::move(between)));
    }
    case AstKind::kInList: {
      ARROW_ASSIGN_OR_RAISE(ExprPtr x, BindExpr(*e.children[0], ctx));
      const LogicalType type = x->type;
      std::vector<std::shared_ptr<arrow::Scalar>> values;
      for (size_t i = 1; i < e.children.size(); ++i) {
        ARROW_ASSIGN_OR_RAISE(ExprPtr item, BindOperand(*e.children[i], ctx, &type));
        if (item->kind != ExprKind::kLiteral) {
          return Err(e.children[i]->pos, "the values of an IN list must be constants");
        }
        values.push_back(item->literal);
      }
      ARROW_ASSIGN_OR_RAISE(ExprPtr in, At(e.pos, MakeInList(std::move(x), values)));
      if (!e.negated) return in;
      return At(e.pos, MakeNot(std::move(in)));
    }
    case AstKind::kIsNull: {
      ARROW_ASSIGN_OR_RAISE(ExprPtr child, BindExpr(*e.children[0], ctx));
      return At(e.pos, MakeIsNull(std::move(child), e.negated));
    }
    case AstKind::kFunction:
      return BindFunction(e, ctx);
  }
  return Err(e.pos, "unsupported expression");
}

// ---- the statement ----

arrow::Result<LogicalPlan> Binder::Run(const ast::SelectStmt& stmt) {
  // FROM: every comma-separated item is bound on its own, then they sit side by side.
  Scope scope;
  std::vector<LogicalPlan> inputs;
  for (const auto& item : stmt.from) {
    ARROW_ASSIGN_OR_RAISE(BoundFrom from, BindFromItem(*item));
    ARROW_RETURN_NOT_OK(AppendScope(&scope, std::move(from.scope)));
    inputs.push_back(std::move(from.plan));
  }
  LogicalPlan plan;
  if (inputs.size() == 1) {
    plan = std::move(inputs[0]);
  } else {
    auto cross = std::make_unique<LogicalCrossProduct>();
    cross->children = std::move(inputs);
    plan = std::move(cross);
  }

  ExprContext row;
  row.scope = &scope;

  if (stmt.where != nullptr) {
    row.clause = Clause::kWhere;
    ARROW_ASSIGN_OR_RAISE(ExprPtr predicate, BindBooleanClause(*stmt.where, row, "WHERE"));
    auto filter = std::make_unique<LogicalFilter>();
    filter->child = std::move(plan);
    filter->predicate = std::move(predicate);
    plan = std::move(filter);
  }

  bool aggregating = !stmt.group_by.empty() || stmt.having != nullptr;
  for (const ast::SelectItem& item : stmt.items)
    aggregating = aggregating || HasAggregate(*item.expr);
  for (const ast::OrderItem& item : stmt.order_by)
    aggregating = aggregating || HasAggregate(*item.expr);

  // GROUP BY expressions, and the context the select list, HAVING and ORDER BY are bound in.
  std::vector<ExprPtr> groups;
  std::vector<std::string> group_keys;
  std::vector<LogicalType> group_types;
  std::vector<AggregateCall> aggregates;
  for (const ast::ExprPtr& g : stmt.group_by) {
    if (g->kind == AstKind::kInteger) {
      return Err(g->pos, "GROUP BY ordinals are not supported: name the expression");
    }
    row.clause = Clause::kGroupBy;
    ARROW_ASSIGN_OR_RAISE(ExprPtr bound, BindExpr(*g, row));
    group_keys.push_back(bound->ToString());
    group_types.push_back(bound->type);
    groups.push_back(std::move(bound));
  }
  ExprContext ctx = row;
  if (aggregating) {
    ctx.post_aggregate = true;
    ctx.group_keys = &group_keys;
    ctx.group_types = &group_types;
    ctx.aggregates = &aggregates;
  }

  // The select list.
  std::vector<ExprPtr> select;
  std::vector<std::string> names;
  std::vector<std::string> select_keys;
  ctx.clause = Clause::kSelect;
  for (const ast::SelectItem& item : stmt.items) {
    ARROW_ASSIGN_OR_RAISE(ExprPtr bound, BindExpr(*item.expr, ctx));
    if (item.has_alias) {
      names.push_back(item.alias_written);
    } else if (item.expr->kind == AstKind::kColumn) {
      names.push_back(item.expr->written);
    } else {
      names.push_back(item.source_text);
    }
    select_keys.push_back(bound->ToString());
    select.push_back(std::move(bound));
  }

  // HAVING.
  ExprPtr having;
  if (stmt.having != nullptr) {
    ctx.clause = Clause::kHaving;
    ARROW_ASSIGN_OR_RAISE(having, BindBooleanClause(*stmt.having, ctx, "HAVING"));
  }

  // ORDER BY: an output alias, else an expression equal to a select item, else a hidden column.
  struct BoundKey {
    ExprPtr expr;  // set for a hidden column
    int output = -1;
    bool ascending = true;
    bool nulls_first = false;
  };
  std::vector<BoundKey> keys;
  size_t hidden = 0;
  ctx.clause = Clause::kOrderBy;
  for (const ast::OrderItem& item : stmt.order_by) {
    const ast::Expr& e = *item.expr;
    if (e.kind == AstKind::kInteger) {
      return Err(e.pos, "ORDER BY ordinals are not supported: name the column or its alias");
    }
    BoundKey key;
    key.ascending = item.ascending;
    key.nulls_first =
        item.nulls == ast::NullsOrder::kFirst;  // NULLS LAST is the default, both ways
    if (e.kind == AstKind::kColumn && !e.has_qualifier) {
      int found = -1;
      int count = 0;
      for (size_t i = 0; i < stmt.items.size(); ++i) {
        if (stmt.items[i].has_alias && stmt.items[i].alias == e.text) {
          found = static_cast<int>(i);
          ++count;
        }
      }
      if (count > 1)
        return Err(e.pos,
                   "ORDER BY " + e.text + " is ambiguous: several select items have that alias");
      key.output = found;
    }
    if (key.output < 0) {
      ARROW_ASSIGN_OR_RAISE(ExprPtr bound, BindExpr(e, ctx));
      const std::string text = bound->ToString();
      for (size_t i = 0; i < select_keys.size(); ++i) {
        if (select_keys[i] == text) {
          key.output = static_cast<int>(i);
          break;
        }
      }
      if (key.output < 0) {
        key.output = static_cast<int>(select.size() + hidden++);
        key.expr = std::move(bound);
      }
    }
    keys.push_back(std::move(key));
  }

  // Assemble: [Aggregate] [HAVING filter] Project [Sort [Project]] [Limit].
  if (aggregating) {
    auto aggregate = std::make_unique<LogicalAggregate>();
    aggregate->child = std::move(plan);
    aggregate->groups = std::move(groups);
    for (AggregateCall& call : aggregates) aggregate->aggregates.push_back(std::move(call.expr));
    plan = std::move(aggregate);
    if (having != nullptr) {
      auto filter = std::make_unique<LogicalFilter>();
      filter->child = std::move(plan);
      filter->predicate = std::move(having);
      plan = std::move(filter);
    }
  }

  const size_t visible = select.size();
  std::vector<LogicalType> visible_types;
  visible_types.reserve(visible);
  for (const ExprPtr& e : select) visible_types.push_back(e->type);
  std::vector<LogicalType> all_types = visible_types;
  auto project = std::make_unique<LogicalProject>();
  project->exprs = std::move(select);
  project->names = names;
  for (BoundKey& key : keys) {
    if (key.expr == nullptr) continue;
    all_types.push_back(key.expr->type);
    project->names.push_back("order_by_" + std::to_string(project->exprs.size()));
    project->exprs.push_back(std::move(key.expr));
  }
  project->child = std::move(plan);
  plan = std::move(project);

  if (!keys.empty()) {
    auto sort = std::make_unique<LogicalSort>();
    for (const BoundKey& key : keys) {
      SortKey sk;
      sk.expr = MakeColumn(key.output, all_types[static_cast<size_t>(key.output)]);
      sk.ascending = key.ascending;
      sk.nulls_first = key.nulls_first;
      sort->keys.push_back(std::move(sk));
    }
    sort->child = std::move(plan);
    plan = std::move(sort);
    if (hidden > 0) {
      auto trim = std::make_unique<LogicalProject>();
      for (size_t i = 0; i < visible; ++i) {
        trim->exprs.push_back(MakeColumn(static_cast<int>(i), visible_types[i]));
        trim->names.push_back(names[i]);
      }
      trim->child = std::move(plan);
      plan = std::move(trim);
    }
  }

  if (stmt.limit.has_value()) {
    auto limit = std::make_unique<LogicalLimit>();
    limit->n = *stmt.limit;
    limit->child = std::move(plan);
    plan = std::move(limit);
  }
  return plan;
}

}  // namespace

arrow::Result<LogicalPlan> Bind(const ast::SelectStmt& stmt, const Catalog& catalog) {
  return Binder(catalog).Run(stmt);
}

arrow::Result<LogicalPlan> ParseAndBind(std::string_view sql, const Catalog& catalog) {
  ParseResult parsed = Parse(sql);
  if (!parsed.ok()) return arrow::Status::Invalid(parsed.error);
  return Bind(*parsed.stmt, catalog);
}

}  // namespace quarry
