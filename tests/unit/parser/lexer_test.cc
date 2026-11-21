#include "parser/lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace quarry {
namespace {

std::vector<Token> Lex(std::string_view sql) {
  LexResult result = Tokenize(sql);
  EXPECT_EQ(result.error, "") << sql;
  return std::move(result.tokens);
}

TEST(LexerTest, KeywordsAreCaseInsensitiveAndUpperCased) {
  const auto tokens = Lex("select SeLeCt FROM where");
  ASSERT_EQ(tokens.size(), 5u);
  for (size_t i = 0; i < 4; ++i) EXPECT_EQ(tokens[i].kind, TokenKind::kKeyword);
  EXPECT_EQ(tokens[0].text, "SELECT");
  EXPECT_EQ(tokens[1].text, "SELECT");
  EXPECT_EQ(tokens[2].text, "FROM");
  EXPECT_EQ(tokens[3].text, "WHERE");
  EXPECT_EQ(tokens[4].kind, TokenKind::kEnd);
}

TEST(LexerTest, IdentifiersKeepTheirSpellingAndRecordQuoting) {
  const auto tokens = Lex(R"(L_Quantity "L_Quantity" "a""b")");
  ASSERT_EQ(tokens.size(), 4u);
  EXPECT_EQ(tokens[0].kind, TokenKind::kIdentifier);
  EXPECT_EQ(tokens[0].text, "L_Quantity");
  EXPECT_FALSE(tokens[0].quoted);
  EXPECT_EQ(tokens[1].kind, TokenKind::kIdentifier);
  EXPECT_EQ(tokens[1].text, "L_Quantity");
  EXPECT_TRUE(tokens[1].quoted);
  EXPECT_EQ(tokens[2].text, "a\"b");
}

TEST(LexerTest, QuotedKeywordIsAnIdentifier) {
  const auto tokens = Lex(R"("select")");
  EXPECT_EQ(tokens[0].kind, TokenKind::kIdentifier);
  EXPECT_EQ(tokens[0].text, "select");
}

TEST(LexerTest, StringEscapeDoublesTheQuote) {
  const auto tokens = Lex("'it''s' ''");
  ASSERT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens[0].kind, TokenKind::kString);
  EXPECT_EQ(tokens[0].text, "it's");
  EXPECT_EQ(tokens[1].kind, TokenKind::kString);
  EXPECT_EQ(tokens[1].text, "");
}

TEST(LexerTest, NumericLiterals) {
  const auto tokens = Lex("1 0.05 .5 007 123.4500");
  ASSERT_EQ(tokens.size(), 6u);
  EXPECT_EQ(tokens[0].kind, TokenKind::kInteger);
  EXPECT_EQ(tokens[0].text, "1");
  EXPECT_EQ(tokens[1].kind, TokenKind::kDecimal);
  EXPECT_EQ(tokens[1].text, "0.05");
  EXPECT_EQ(tokens[2].kind, TokenKind::kDecimal);
  EXPECT_EQ(tokens[2].text, ".5");
  EXPECT_EQ(tokens[3].kind, TokenKind::kInteger);
  EXPECT_EQ(tokens[3].text, "007");
  EXPECT_EQ(tokens[4].kind, TokenKind::kDecimal);
  EXPECT_EQ(tokens[4].text, "123.4500");
}

TEST(LexerTest, OperatorsAndPunctuation) {
  const auto tokens = Lex("+ - * / = <> != < <= > >= ( ) , . ;");
  const std::vector<TokenKind> expected = {
      TokenKind::kPlus,   TokenKind::kMinus, TokenKind::kStar, TokenKind::kSlash,
      TokenKind::kEq,     TokenKind::kNe,    TokenKind::kNe,   TokenKind::kLt,
      TokenKind::kLe,     TokenKind::kGt,    TokenKind::kGe,   TokenKind::kLParen,
      TokenKind::kRParen, TokenKind::kComma, TokenKind::kDot,  TokenKind::kSemicolon,
      TokenKind::kEnd};
  ASSERT_EQ(tokens.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(tokens[i].kind, expected[i]) << i;
}

TEST(LexerTest, CommentsRunToTheEndOfTheLine) {
  const auto tokens = Lex("select -- a comment, 'not a string\n 1 -- trailing");
  ASSERT_EQ(tokens.size(), 3u);
  EXPECT_EQ(tokens[1].kind, TokenKind::kInteger);
  EXPECT_EQ(tokens[1].line, 2);
  EXPECT_EQ(tokens[1].column, 2);
}

TEST(LexerTest, PositionsAreOneBasedLineAndColumn) {
  const auto tokens = Lex("select a,\n  b");
  ASSERT_EQ(tokens.size(), 5u);
  EXPECT_EQ(tokens[0].line, 1);
  EXPECT_EQ(tokens[0].column, 1);
  EXPECT_EQ(tokens[1].column, 8);
  EXPECT_EQ(tokens[3].line, 2);
  EXPECT_EQ(tokens[3].column, 3);
  EXPECT_EQ(tokens[3].offset, 12u);
  EXPECT_EQ(tokens[3].length, 1u);
}

TEST(LexerTest, UnterminatedStringIsASyntaxErrorAtItsOpeningQuote) {
  const LexResult result = Tokenize("select 'abc");
  EXPECT_EQ(result.error.rfind("syntax error at 1:8: ", 0), 0u) << result.error;
}

TEST(LexerTest, UnterminatedQuotedIdentifier) {
  const LexResult result = Tokenize("select \"abc");
  EXPECT_EQ(result.error.rfind("syntax error at 1:8: ", 0), 0u) << result.error;
}

TEST(LexerTest, UnknownCharacterAndMalformedNumber) {
  EXPECT_EQ(Tokenize("select #").error.rfind("syntax error at 1:8: ", 0), 0u);
  EXPECT_EQ(Tokenize("select 12abc").error.rfind("syntax error at 1:8: ", 0), 0u);
  EXPECT_EQ(Tokenize("select 1e3").error.rfind("syntax error at 1:8: ", 0), 0u);
  EXPECT_EQ(Tokenize("select \"\"").error.rfind("syntax error at 1:8: ", 0), 0u);
}

TEST(LexerTest, ControlAndHighBytesOutsideStringsAreErrorsNotCrashes) {
  const std::string with_nul("select \0 1", 10);
  EXPECT_EQ(Tokenize(with_nul).error.rfind("syntax error at 1:8: ", 0), 0u);
  EXPECT_FALSE(Tokenize("select \xff").error.empty());
  // Inside a string any byte is data.
  EXPECT_EQ(Tokenize("select '\xff\x01'").error, "");
}

TEST(LexerTest, KeywordTables) {
  EXPECT_TRUE(IsKeyword("SELECT"));
  EXPECT_TRUE(IsKeyword("LEFT"));
  EXPECT_FALSE(IsKeyword("LINEITEM"));
  EXPECT_TRUE(IsUnsupportedKeyword("LEFT"));
  EXPECT_FALSE(IsUnsupportedKeyword("SELECT"));
}

}  // namespace
}  // namespace quarry
