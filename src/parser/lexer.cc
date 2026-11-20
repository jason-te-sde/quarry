#include "parser/lexer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace quarry {
namespace {

constexpr auto kSupportedKeywords = std::to_array<std::string_view>(
    {"SELECT", "FROM",  "WHERE", "GROUP", "BY",      "HAVING", "ORDER", "LIMIT",
     "AS",     "AND",   "OR",    "NOT",   "BETWEEN", "IN",     "IS",    "NULL",
     "JOIN",   "INNER", "ON",    "ASC",   "DESC",    "TRUE",   "FALSE"});

// Words of SQL that quarry does not implement. They are reserved so that using one is an error
// that says so, instead of the word being taken for a table or column name.
constexpr auto kUnsupportedKeywords = std::to_array<std::string_view>(
    {"LEFT",   "RIGHT",    "FULL", "OUTER", "CROSS", "NATURAL", "UNION",  "INTERSECT",
     "EXCEPT", "DISTINCT", "LIKE", "CASE",  "WHEN",  "THEN",    "ELSE",   "END",
     "EXISTS", "ALL",      "ANY",  "WITH",  "OVER",  "USING",   "OFFSET", "ILIKE"});

bool IsIdentStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsIdentPart(char c) { return IsIdentStart(c) || IsDigit(c); }

std::string Upper(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

// A character for a message: printable ASCII as itself, anything else as \xNN.
std::string Printable(char c) {
  const auto u = static_cast<unsigned char>(c);
  if (u >= 0x20 && u < 0x7f) return std::string(1, c);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out = "\\x";
  out += kHex[u >> 4];
  out += kHex[u & 15];
  return out;
}

class Lexer {
 public:
  explicit Lexer(std::string_view sql) : sql_(sql) {}

  LexResult Run() {
    LexResult result;
    while (true) {
      SkipSpaceAndComments();
      if (pos_ >= sql_.size()) break;
      Token token;
      token.line = line_;
      token.column = column_;
      token.offset = pos_;
      if (!Next(&token)) {
        result.tokens.clear();
        result.error = error_;
        return result;
      }
      token.length = pos_ - token.offset;
      result.tokens.push_back(std::move(token));
    }
    Token end;
    end.kind = TokenKind::kEnd;
    end.line = line_;
    end.column = column_;
    end.offset = sql_.size();
    result.tokens.push_back(std::move(end));
    return result;
  }

 private:
  char Peek(size_t ahead = 0) const {
    return pos_ + ahead < sql_.size() ? sql_[pos_ + ahead] : '\0';
  }
  bool AtEnd(size_t ahead = 0) const { return pos_ + ahead >= sql_.size(); }

  void Advance() {
    if (sql_[pos_] == '\n') {
      ++line_;
      column_ = 1;
    } else {
      ++column_;
    }
    ++pos_;
  }

  void SkipSpaceAndComments() {
    while (!AtEnd()) {
      const char c = Peek();
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
        Advance();
      } else if (c == '-' && Peek(1) == '-') {
        while (!AtEnd() && Peek() != '\n') Advance();
      } else {
        break;
      }
    }
  }

  bool Fail(const Token& at, const std::string& message) {
    error_ = "syntax error at " + std::to_string(at.line) + ":" + std::to_string(at.column) + ": " +
             message;
    return false;
  }

  bool Next(Token* token) {
    const char c = Peek();
    if (IsIdentStart(c)) return Word(token);
    if (IsDigit(c) || (c == '.' && IsDigit(Peek(1)))) return Number(token);
    if (c == '\'') return Quoted(token, '\'', TokenKind::kString);
    if (c == '"') return Quoted(token, '"', TokenKind::kIdentifier);
    return Punctuation(token);
  }

  bool Word(Token* token) {
    const size_t start = pos_;
    while (!AtEnd() && IsIdentPart(Peek())) Advance();
    const std::string_view word = sql_.substr(start, pos_ - start);
    const std::string upper = Upper(word);
    if (IsKeyword(upper)) {
      token->kind = TokenKind::kKeyword;
      token->text = upper;
    } else {
      token->kind = TokenKind::kIdentifier;
      token->text = std::string(word);
    }
    return true;
  }

  bool Number(Token* token) {
    const size_t start = pos_;
    bool decimal = false;
    while (IsDigit(Peek())) Advance();
    if (Peek() == '.' && IsDigit(Peek(1))) {
      decimal = true;
      Advance();
      while (IsDigit(Peek())) Advance();
    }
    if (IsIdentPart(Peek())) {
      return Fail(*token, "invalid numeric literal: a number may not be followed by '" +
                              Printable(Peek()) + "'");
    }
    token->kind = decimal ? TokenKind::kDecimal : TokenKind::kInteger;
    token->text = std::string(sql_.substr(start, pos_ - start));
    return true;
  }

  // A string or a quoted name: `quote` doubled inside stands for one `quote`.
  bool Quoted(Token* token, char quote, TokenKind kind) {
    Advance();  // the opening quote
    std::string value;
    while (true) {
      if (AtEnd()) {
        return Fail(*token, kind == TokenKind::kString ? "unterminated string literal"
                                                       : "unterminated quoted identifier");
      }
      const char c = Peek();
      if (c == quote) {
        if (Peek(1) == quote) {
          value += quote;
          Advance();
          Advance();
          continue;
        }
        Advance();
        break;
      }
      value += c;
      Advance();
    }
    if (kind == TokenKind::kIdentifier && value.empty()) {
      return Fail(*token, "a quoted identifier may not be empty");
    }
    token->kind = kind;
    token->quoted = kind == TokenKind::kIdentifier;
    token->text = std::move(value);
    return true;
  }

  bool Punctuation(Token* token) {
    const char c = Peek();
    const char next = Peek(1);
    TokenKind kind;
    size_t width = 1;
    switch (c) {
      case '+':
        kind = TokenKind::kPlus;
        break;
      case '-':
        kind = TokenKind::kMinus;
        break;
      case '*':
        kind = TokenKind::kStar;
        break;
      case '/':
        kind = TokenKind::kSlash;
        break;
      case '=':
        kind = TokenKind::kEq;
        break;
      case '(':
        kind = TokenKind::kLParen;
        break;
      case ')':
        kind = TokenKind::kRParen;
        break;
      case ',':
        kind = TokenKind::kComma;
        break;
      case '.':
        kind = TokenKind::kDot;
        break;
      case ';':
        kind = TokenKind::kSemicolon;
        break;
      case '<':
        if (next == '>') {
          kind = TokenKind::kNe;
          width = 2;
        } else if (next == '=') {
          kind = TokenKind::kLe;
          width = 2;
        } else {
          kind = TokenKind::kLt;
        }
        break;
      case '>':
        if (next == '=') {
          kind = TokenKind::kGe;
          width = 2;
        } else {
          kind = TokenKind::kGt;
        }
        break;
      case '!':
        if (next != '=') return Fail(*token, "unexpected character '!'");
        kind = TokenKind::kNe;
        width = 2;
        break;
      default:
        return Fail(*token, "unexpected character '" + Printable(c) + "'");
    }
    token->kind = kind;
    token->text = std::string(sql_.substr(pos_, width));
    for (size_t i = 0; i < width; ++i) Advance();
    return true;
  }

  std::string_view sql_;
  size_t pos_ = 0;
  int line_ = 1;
  int column_ = 1;
  std::string error_;
};

}  // namespace

LexResult Tokenize(std::string_view sql) { return Lexer(sql).Run(); }

bool IsKeyword(std::string_view upper_case_word) {
  return std::find(kSupportedKeywords.begin(), kSupportedKeywords.end(), upper_case_word) !=
             kSupportedKeywords.end() ||
         IsUnsupportedKeyword(upper_case_word);
}

bool IsUnsupportedKeyword(std::string_view upper_case_word) {
  return std::find(kUnsupportedKeywords.begin(), kUnsupportedKeywords.end(), upper_case_word) !=
         kUnsupportedKeywords.end();
}

}  // namespace quarry
