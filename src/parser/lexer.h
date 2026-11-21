#ifndef QUARRY_PARSER_LEXER_H_
#define QUARRY_PARSER_LEXER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace quarry {

enum class TokenKind : uint8_t {
  kEnd,
  kIdentifier,  // an unquoted or quoted name; `quoted` tells which
  kKeyword,     // a reserved word; `text` is upper-case
  kInteger,     // digits only
  kDecimal,     // digits '.' digits, or '.' digits
  kString,      // text is the unescaped value
  kPlus,
  kMinus,
  kStar,
  kSlash,
  kEq,
  kNe,  // <> and !=
  kLt,
  kLe,
  kGt,
  kGe,
  kLParen,
  kRParen,
  kComma,
  kDot,
  kSemicolon,
};

struct Token {
  TokenKind kind = TokenKind::kEnd;
  // kIdentifier: the name as written (quotes removed, "" unescaped). kKeyword: upper-case.
  // kInteger, kDecimal: the digits. kString: the value.
  std::string text;
  bool quoted = false;
  int line = 1;
  int column = 1;
  size_t offset = 0;  // byte offset of the first character in the input
  size_t length = 0;  // bytes the token spans in the input
};

struct LexResult {
  std::vector<Token> tokens;  // always ends with a kEnd token when error is empty
  std::string error;          // "syntax error at L:C: ..." or empty
};

// Splits `sql` into tokens. Keywords are case-insensitive; `--` starts a comment that runs to the
// end of the line. An unterminated string or quoted name, an unknown character and a malformed
// number are errors reported at the position where the offending token starts.
LexResult Tokenize(std::string_view sql);

// True for the words that are reserved. Includes the SQL words quarry does not support, so that
// they are refused with a clear message instead of being taken for a name.
bool IsKeyword(std::string_view upper_case_word);
// True for reserved words that name SQL features outside the supported subset (LEFT, UNION, ...).
bool IsUnsupportedKeyword(std::string_view upper_case_word);

}  // namespace quarry

#endif  // QUARRY_PARSER_LEXER_H_
