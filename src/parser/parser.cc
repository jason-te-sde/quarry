#include "parser/parser.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "parser/lexer.h"

namespace quarry {
namespace {

using ast::Expr;
using ast::ExprKind;
using ast::ExprPtr;

// Binding powers, lowest to highest.
constexpr int kPrecOr = 1;
constexpr int kPrecAnd = 2;
// A prefix NOT sits between AND and the comparisons: its operand is parsed at kPrecComparison.
constexpr int kPrecComparison = 4;
constexpr int kPrecAdditive = 5;
constexpr int kPrecMultiplicative = 6;

std::string Lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

class Parser {
 public:
  Parser(std::string_view sql, std::vector<Token> tokens) : sql_(sql), tokens_(std::move(tokens)) {}

  ParseResult Run() {
    ParseResult result;
    auto stmt = ParseSelect();
    if (stmt != nullptr) {
      Accept(TokenKind::kSemicolon);
      if (Cur().kind != TokenKind::kEnd) {
        Unexpected(Cur(), "end of input");
        stmt = nullptr;
      }
    }
    if (stmt == nullptr) {
      result.error = error_;
    } else {
      result.stmt = std::move(stmt);
    }
    return result;
  }

 private:
  // ---- token access ----
  const Token& Cur() const { return tokens_[index_]; }
  const Token& PeekAt(size_t ahead) const {
    return tokens_[std::min(index_ + ahead, tokens_.size() - 1)];
  }
  const Token& Prev() const { return tokens_[index_ - 1]; }
  void Advance() {
    if (index_ + 1 < tokens_.size()) ++index_;
  }
  bool Accept(TokenKind kind) {
    if (Cur().kind != kind) return false;
    Advance();
    return true;
  }
  static bool IsKeyword(const Token& t, const char* word) {
    return t.kind == TokenKind::kKeyword && t.text == word;
  }
  // An unquoted identifier spelling `word` (lower-case): the contextual words NULLS, FIRST, ...
  static bool IsWord(const Token& t, const char* word) {
    return t.kind == TokenKind::kIdentifier && !t.quoted && Lower(t.text) == word;
  }
  bool AcceptKeyword(const char* word) {
    if (!IsKeyword(Cur(), word)) return false;
    Advance();
    return true;
  }
  bool ExpectKeyword(const char* word) {
    if (AcceptKeyword(word)) return true;
    Unexpected(Cur(), word);
    return false;
  }

  // ---- errors: the first one wins, and every parse function then returns null ----
  static std::string Describe(const Token& t) {
    switch (t.kind) {
      case TokenKind::kEnd:
        return "end of input";
      case TokenKind::kKeyword:
        return "keyword " + t.text;
      case TokenKind::kIdentifier:
        return "identifier '" + t.text + "'";
      case TokenKind::kInteger:
      case TokenKind::kDecimal:
        return "number '" + t.text + "'";
      case TokenKind::kString:
        return "string literal";
      default:
        return "'" + t.text + "'";
    }
  }

  void FailAt(const Token& at, const std::string& message) {
    if (!error_.empty()) return;
    error_ = "syntax error at " + std::to_string(at.line) + ":" + std::to_string(at.column) + ": " +
             message;
  }

  void Unexpected(const Token& t, const std::string& expected) {
    if (t.kind == TokenKind::kSlash) {
      FailAt(t, "division is not supported");
    } else if (t.kind == TokenKind::kKeyword && IsUnsupportedKeyword(t.text)) {
      FailAt(t, t.text + " is not supported");
    } else {
      FailAt(t, "unexpected " + Describe(t) + ", expected " + expected);
    }
  }

  // Nesting depth of the recursive descent; the limit keeps the C++ stack bounded.
  class DepthGuard {
   public:
    explicit DepthGuard(Parser* p) : parser_(p) {
      if (++parser_->depth_ > kMaxNestingDepth) {
        parser_->FailAt(parser_->Cur(),
                        "nesting too deep (limit " + std::to_string(kMaxNestingDepth) + ")");
      }
    }
    ~DepthGuard() { --parser_->depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;
    bool ok() const { return parser_->depth_ <= kMaxNestingDepth; }

   private:
    Parser* parser_;
  };

  static ast::Position PosOf(const Token& t) { return {t.line, t.column}; }

  ExprPtr NewExpr(ExprKind kind, const Token& at) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->pos = PosOf(at);
    return e;
  }

  // Sets the height of `e` from its children and refuses a tree that is too tall.
  bool Seal(Expr* e, const Token& at) {
    int height = 0;
    for (const ExprPtr& child : e->children) height = std::max(height, child->depth);
    e->depth = height + 1;
    if (e->depth > kMaxNestingDepth) {
      FailAt(at, "expression nesting too deep (limit " + std::to_string(kMaxNestingDepth) + ")");
      return false;
    }
    return true;
  }

  // ---- statement ----
  std::unique_ptr<ast::SelectStmt> ParseSelect() {
    if (!ExpectKeyword("SELECT")) return nullptr;
    auto stmt = std::make_unique<ast::SelectStmt>();
    do {
      if (!ParseSelectItem(stmt.get())) return nullptr;
    } while (Accept(TokenKind::kComma));

    if (!ExpectKeyword("FROM")) return nullptr;
    do {
      auto item = ParseFromItem();
      if (item == nullptr) return nullptr;
      stmt->from.push_back(std::move(item));
    } while (Accept(TokenKind::kComma));

    if (AcceptKeyword("WHERE")) {
      stmt->where = ParseExpr(kPrecOr);
      if (stmt->where == nullptr) return nullptr;
    }
    if (AcceptKeyword("GROUP")) {
      if (!ExpectKeyword("BY")) return nullptr;
      do {
        ExprPtr e = ParseExpr(kPrecOr);
        if (e == nullptr) return nullptr;
        stmt->group_by.push_back(std::move(e));
      } while (Accept(TokenKind::kComma));
    }
    if (AcceptKeyword("HAVING")) {
      stmt->having = ParseExpr(kPrecOr);
      if (stmt->having == nullptr) return nullptr;
    }
    if (AcceptKeyword("ORDER")) {
      if (!ExpectKeyword("BY")) return nullptr;
      do {
        if (!ParseOrderItem(stmt.get())) return nullptr;
      } while (Accept(TokenKind::kComma));
    }
    if (IsKeyword(Cur(), "LIMIT")) {
      Advance();
      const Token& n = Cur();
      if (n.kind != TokenKind::kInteger) {
        Unexpected(n, "an integer");
        return nullptr;
      }
      int64_t value = 0;
      for (const char c : n.text) {
        const int digit = c - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
          FailAt(n, "LIMIT value is too large");
          return nullptr;
        }
        value = value * 10 + digit;
      }
      stmt->limit = value;
      stmt->limit_pos = PosOf(n);
      Advance();
    }
    return stmt;
  }

  bool ParseSelectItem(ast::SelectStmt* stmt) {
    if (Cur().kind == TokenKind::kStar) {
      FailAt(Cur(), "SELECT * is not supported: list the columns");
      return false;
    }
    ast::SelectItem item;
    const Token& first = Cur();
    item.pos = PosOf(first);
    item.expr = ParseExpr(kPrecOr);
    if (item.expr == nullptr) return false;
    const Token& last = Prev();
    item.source_text =
        std::string(sql_.substr(first.offset, last.offset + last.length - first.offset));
    if (AcceptKeyword("AS")) {
      if (Cur().kind != TokenKind::kIdentifier) {
        Unexpected(Cur(), "an alias");
        return false;
      }
      item.has_alias = true;
    } else if (Cur().kind == TokenKind::kIdentifier) {
      item.has_alias = true;
    }
    if (item.has_alias) {
      item.alias_quoted = Cur().quoted;
      item.alias = item.alias_quoted ? Cur().text : Lower(Cur().text);
      item.alias_written = Cur().text;
      Advance();
    }
    stmt->items.push_back(std::move(item));
    return true;
  }

  std::unique_ptr<ast::FromItem> ParseTable() {
    const Token& t = Cur();
    if (t.kind != TokenKind::kIdentifier) {
      if (t.kind == TokenKind::kLParen) {
        FailAt(t, "subqueries and parenthesised FROM items are not supported");
      } else {
        Unexpected(t, "a table name");
      }
      return nullptr;
    }
    auto item = std::make_unique<ast::FromItem>();
    item->kind = ast::FromItem::Kind::kTable;
    item->pos = PosOf(t);
    item->table_quoted = t.quoted;
    item->table = t.quoted ? t.text : Lower(t.text);
    Advance();
    if (AcceptKeyword("AS")) {
      if (Cur().kind != TokenKind::kIdentifier) {
        Unexpected(Cur(), "an alias");
        return nullptr;
      }
      item->has_alias = true;
    } else if (Cur().kind == TokenKind::kIdentifier) {
      item->has_alias = true;
    }
    if (item->has_alias) {
      item->alias_quoted = Cur().quoted;
      item->alias = item->alias_quoted ? Cur().text : Lower(Cur().text);
      Advance();
    }
    return item;
  }

  std::unique_ptr<ast::FromItem> ParseFromItem() {
    auto left = ParseTable();
    if (left == nullptr) return nullptr;
    int joins = 0;
    while (IsKeyword(Cur(), "JOIN") || IsKeyword(Cur(), "INNER")) {
      const Token& join = Cur();
      if (AcceptKeyword("INNER") && !ExpectKeyword("JOIN")) return nullptr;
      if (IsKeyword(Cur(), "JOIN")) Advance();
      if (++joins > kMaxNestingDepth) {
        FailAt(join, "join nesting too deep (limit " + std::to_string(kMaxNestingDepth) + ")");
        return nullptr;
      }
      auto right = ParseTable();
      if (right == nullptr) return nullptr;
      if (!ExpectKeyword("ON")) return nullptr;
      ExprPtr on = ParseExpr(kPrecOr);
      if (on == nullptr) return nullptr;
      auto node = std::make_unique<ast::FromItem>();
      node->kind = ast::FromItem::Kind::kJoin;
      node->pos = PosOf(join);
      node->left = std::move(left);
      node->right = std::move(right);
      node->on = std::move(on);
      left = std::move(node);
    }
    return left;
  }

  bool ParseOrderItem(ast::SelectStmt* stmt) {
    ast::OrderItem item;
    item.expr = ParseExpr(kPrecOr);
    if (item.expr == nullptr) return false;
    if (AcceptKeyword("DESC")) {
      item.ascending = false;
    } else {
      AcceptKeyword("ASC");
    }
    if (IsWord(Cur(), "nulls")) {
      Advance();
      if (IsWord(Cur(), "first")) {
        item.nulls = ast::NullsOrder::kFirst;
      } else if (IsWord(Cur(), "last")) {
        item.nulls = ast::NullsOrder::kLast;
      } else {
        Unexpected(Cur(), "FIRST or LAST");
        return false;
      }
      Advance();
    }
    stmt->order_by.push_back(std::move(item));
    return true;
  }

  // ---- expressions: precedence climbing ----
  // Returns the binding power of the binary operator at the cursor, or 0 when there is none.
  int BinaryPrecedence(const Token& t) const {
    switch (t.kind) {
      case TokenKind::kPlus:
      case TokenKind::kMinus:
        return kPrecAdditive;
      case TokenKind::kStar:
      case TokenKind::kSlash:
        return kPrecMultiplicative;
      case TokenKind::kEq:
      case TokenKind::kNe:
      case TokenKind::kLt:
      case TokenKind::kLe:
      case TokenKind::kGt:
      case TokenKind::kGe:
        return kPrecComparison;
      case TokenKind::kKeyword:
        if (t.text == "OR") return kPrecOr;
        if (t.text == "AND") return kPrecAnd;
        if (t.text == "IS" || t.text == "BETWEEN" || t.text == "IN") return kPrecComparison;
        if (t.text == "NOT") {
          const Token& next = PeekAt(1);
          if (IsKeyword(next, "BETWEEN") || IsKeyword(next, "IN")) return kPrecComparison;
        }
        return 0;
      default:
        return 0;
    }
  }

  static ast::BinOp OperatorOf(const Token& t) {
    switch (t.kind) {
      case TokenKind::kPlus:
        return ast::BinOp::kAdd;
      case TokenKind::kMinus:
        return ast::BinOp::kSub;
      case TokenKind::kStar:
        return ast::BinOp::kMul;
      case TokenKind::kEq:
        return ast::BinOp::kEq;
      case TokenKind::kNe:
        return ast::BinOp::kNe;
      case TokenKind::kLt:
        return ast::BinOp::kLt;
      case TokenKind::kLe:
        return ast::BinOp::kLe;
      case TokenKind::kGt:
        return ast::BinOp::kGt;
      case TokenKind::kGe:
        return ast::BinOp::kGe;
      default:
        return t.text == "AND" ? ast::BinOp::kAnd : ast::BinOp::kOr;
    }
  }

  ExprPtr ParseExpr(int min_prec) {
    DepthGuard guard(this);
    if (!guard.ok()) return nullptr;
    ExprPtr lhs = ParseUnary();
    if (lhs == nullptr) return nullptr;
    while (true) {
      const Token& op = Cur();
      if (op.kind == TokenKind::kSlash && min_prec <= kPrecMultiplicative) {
        FailAt(op, "division is not supported");
        return nullptr;
      }
      const int prec = BinaryPrecedence(op);
      if (prec == 0 || prec < min_prec) break;
      if (prec == kPrecComparison && (IsKeyword(op, "IS") || IsKeyword(op, "BETWEEN") ||
                                      IsKeyword(op, "IN") || IsKeyword(op, "NOT"))) {
        lhs = ParsePostfixPredicate(std::move(lhs));
      } else {
        Advance();
        ExprPtr rhs = ParseExpr(prec + 1);
        if (rhs == nullptr) return nullptr;
        auto node = NewExpr(ExprKind::kBinary, op);
        node->binary_op = OperatorOf(op);
        node->children.push_back(std::move(lhs));
        node->children.push_back(std::move(rhs));
        if (!Seal(node.get(), op)) return nullptr;
        lhs = std::move(node);
      }
      if (lhs == nullptr) return nullptr;
    }
    return lhs;
  }

  // IS [NOT] NULL, [NOT] BETWEEN ... AND ..., [NOT] IN (...), with `lhs` already parsed.
  ExprPtr ParsePostfixPredicate(ExprPtr lhs) {
    const Token& start = Cur();
    if (AcceptKeyword("IS")) {
      const bool negated = AcceptKeyword("NOT");
      if (!ExpectKeyword("NULL")) return nullptr;
      auto node = NewExpr(ExprKind::kIsNull, start);
      node->negated = negated;
      node->children.push_back(std::move(lhs));
      return Seal(node.get(), start) ? std::move(node) : nullptr;
    }
    const bool negated = AcceptKeyword("NOT");
    if (AcceptKeyword("BETWEEN")) {
      ExprPtr lo = ParseExpr(kPrecAdditive);
      if (lo == nullptr || !ExpectKeyword("AND")) return nullptr;
      ExprPtr hi = ParseExpr(kPrecAdditive);
      if (hi == nullptr) return nullptr;
      auto node = NewExpr(ExprKind::kBetween, start);
      node->negated = negated;
      node->children.push_back(std::move(lhs));
      node->children.push_back(std::move(lo));
      node->children.push_back(std::move(hi));
      return Seal(node.get(), start) ? std::move(node) : nullptr;
    }
    if (!ExpectKeyword("IN")) return nullptr;
    if (!Accept(TokenKind::kLParen)) {
      Unexpected(Cur(), "'('");
      return nullptr;
    }
    auto node = NewExpr(ExprKind::kInList, start);
    node->negated = negated;
    node->children.push_back(std::move(lhs));
    do {
      ExprPtr item = ParseExpr(kPrecOr);
      if (item == nullptr) return nullptr;
      node->children.push_back(std::move(item));
    } while (Accept(TokenKind::kComma));
    if (!Accept(TokenKind::kRParen)) {
      Unexpected(Cur(), "',' or ')'");
      return nullptr;
    }
    return Seal(node.get(), start) ? std::move(node) : nullptr;
  }

  ExprPtr ParseUnary() {
    const Token& t = Cur();
    if (t.kind == TokenKind::kMinus || IsKeyword(t, "NOT") || t.kind == TokenKind::kPlus) {
      // A prefix operator recurses, so it counts as a level; a primary does not (ParseExpr, which
      // a parenthesis re-enters, has already counted).
      DepthGuard guard(this);
      if (!guard.ok()) return nullptr;
      return ParsePrefix();
    }
    return ParsePrimary();
  }

  ExprPtr ParsePrefix() {
    const Token& t = Cur();
    if (t.kind == TokenKind::kMinus || IsKeyword(t, "NOT")) {
      const bool is_not = t.kind != TokenKind::kMinus;
      Advance();
      ExprPtr operand = is_not ? ParseExpr(kPrecComparison) : ParseUnary();
      if (operand == nullptr) return nullptr;
      auto node = NewExpr(ExprKind::kUnary, t);
      node->unary_op = is_not ? ast::UnaryOp::kNot : ast::UnaryOp::kNegate;
      node->children.push_back(std::move(operand));
      return Seal(node.get(), t) ? std::move(node) : nullptr;
    }
    Advance();  // unary plus
    return ParseUnary();
  }

  ExprPtr ParsePrimary() {
    const Token& t = Cur();
    switch (t.kind) {
      case TokenKind::kInteger:
      case TokenKind::kDecimal: {
        auto e =
            NewExpr(t.kind == TokenKind::kInteger ? ExprKind::kInteger : ExprKind::kDecimal, t);
        e->text = t.text;
        Advance();
        return e;
      }
      case TokenKind::kString: {
        auto e = NewExpr(ExprKind::kString, t);
        e->text = t.text;
        Advance();
        return e;
      }
      case TokenKind::kLParen: {
        Advance();
        if (IsKeyword(Cur(), "SELECT")) {
          FailAt(Cur(), "subqueries are not supported");
          return nullptr;
        }
        ExprPtr inner = ParseExpr(kPrecOr);
        if (inner == nullptr) return nullptr;
        if (!Accept(TokenKind::kRParen)) {
          Unexpected(Cur(), "')'");
          return nullptr;
        }
        return inner;
      }
      case TokenKind::kKeyword:
        if (t.text == "NULL") {
          Advance();
          return NewExpr(ExprKind::kNull, t);
        }
        if (t.text == "TRUE" || t.text == "FALSE") {
          auto e = NewExpr(ExprKind::kBoolean, t);
          e->boolean_value = t.text == "TRUE";
          Advance();
          return e;
        }
        break;
      case TokenKind::kIdentifier:
        return ParseIdentifierExpr();
      default:
        break;
    }
    Unexpected(t, "an expression");
    return nullptr;
  }

  ExprPtr ParseIdentifierExpr() {
    const Token& t = Cur();
    const Token& next = PeekAt(1);
    if (IsWord(t, "date") && next.kind == TokenKind::kString) {
      auto e = NewExpr(ExprKind::kDate, t);
      e->text = next.text;
      Advance();
      Advance();
      return e;
    }
    if (IsWord(t, "interval") && next.kind == TokenKind::kString) return ParseInterval();
    if (next.kind == TokenKind::kLParen) return ParseFunction();

    auto e = NewExpr(ExprKind::kColumn, t);
    e->quoted = t.quoted;
    e->text = t.quoted ? t.text : Lower(t.text);
    e->written = t.text;
    Advance();
    if (Cur().kind == TokenKind::kDot && PeekAt(1).kind == TokenKind::kIdentifier) {
      Advance();
      e->has_qualifier = true;
      e->qualifier = e->text;
      e->qualifier_quoted = e->quoted;
      e->quoted = Cur().quoted;
      e->text = Cur().quoted ? Cur().text : Lower(Cur().text);
      e->written = Cur().text;
      Advance();
    }
    return e;
  }

  ExprPtr ParseInterval() {
    const Token& t = Cur();
    const Token& value = PeekAt(1);
    auto e = NewExpr(ExprKind::kInterval, t);
    // The value is an optionally signed integer.
    size_t i = 0;
    bool negative = false;
    if (!value.text.empty() && (value.text[0] == '-' || value.text[0] == '+')) {
      negative = value.text[0] == '-';
      i = 1;
    }
    int64_t count = 0;
    bool valid = i < value.text.size();
    for (; valid && i < value.text.size(); ++i) {
      const char c = value.text[i];
      if (c < '0' || c > '9' || count > 100000000) {
        valid = false;
        break;
      }
      count = count * 10 + (c - '0');
    }
    if (!valid) {
      FailAt(value,
             "an INTERVAL value must be an integer of at most nine digits, as in "
             "INTERVAL '90' DAY");
      return nullptr;
    }
    e->interval_count = negative ? -count : count;
    Advance();
    Advance();
    const Token& unit = Cur();
    if (IsWord(unit, "day")) {
      e->interval_unit = ast::IntervalUnit::kDay;
    } else if (IsWord(unit, "month")) {
      e->interval_unit = ast::IntervalUnit::kMonth;
    } else if (IsWord(unit, "year")) {
      e->interval_unit = ast::IntervalUnit::kYear;
    } else {
      Unexpected(unit, "DAY, MONTH or YEAR");
      return nullptr;
    }
    Advance();
    return e;
  }

  ExprPtr ParseFunction() {
    const Token& name = Cur();
    auto e = NewExpr(ExprKind::kFunction, name);
    e->text = Lower(name.text);
    Advance();  // the name
    Advance();  // '('
    if (Accept(TokenKind::kStar)) {
      e->star = true;
    } else if (Cur().kind != TokenKind::kRParen) {
      do {
        ExprPtr arg = ParseExpr(kPrecOr);
        if (arg == nullptr) return nullptr;
        e->children.push_back(std::move(arg));
      } while (Accept(TokenKind::kComma));
    }
    if (!Accept(TokenKind::kRParen)) {
      Unexpected(Cur(), "')'");
      return nullptr;
    }
    return Seal(e.get(), name) ? std::move(e) : nullptr;
  }

  std::string_view sql_;
  std::vector<Token> tokens_;
  size_t index_ = 0;
  int depth_ = 0;
  std::string error_;
};

}  // namespace

ParseResult Parse(std::string_view sql) {
  LexResult lexed = Tokenize(sql);
  if (!lexed.error.empty()) {
    ParseResult result;
    result.error = std::move(lexed.error);
    return result;
  }
  return Parser(sql, std::move(lexed.tokens)).Run();
}

}  // namespace quarry
