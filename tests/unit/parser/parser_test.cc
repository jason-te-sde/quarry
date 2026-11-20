#include "parser/parser.h"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

#include "parser/ast.h"

namespace quarry {
namespace {

std::unique_ptr<ast::SelectStmt> MustParse(std::string_view sql) {
  ParseResult result = Parse(sql);
  EXPECT_TRUE(result.ok()) << sql << "\n" << result.error;
  return std::move(result.stmt);
}

// The s-expression of the expression of `SELECT <expr> FROM t`.
std::string ExprOf(std::string_view expr) {
  const auto stmt = MustParse("select " + std::string(expr) + " from t");
  if (stmt == nullptr || stmt->items.size() != 1) return "<parse failed>";
  return stmt->items[0].expr->ToString();
}

// "- - - ... ": a chain of unary minus signs (two adjacent minus signs would start a comment).
std::string MinusChain(int n) {
  std::string out;
  for (int i = 0; i < n; ++i) out += "- ";
  return out;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(ParserTest, EveryTpchQueryFileParses) {
  for (const char* q : {"q1", "q3", "q5", "q6"}) {
    const std::string sql = ReadFile(std::string("tests/tpch/queries/") + q + ".sql");
    ASSERT_FALSE(sql.empty());
    ParseResult result = Parse(sql);
    EXPECT_TRUE(result.ok()) << q << ": " << result.error;
  }
}

TEST(ParserTest, Q3HasTheExpectedShape) {
  const auto stmt = MustParse(ReadFile("tests/tpch/queries/q3.sql"));
  ASSERT_NE(stmt, nullptr);
  ASSERT_EQ(stmt->items.size(), 4u);
  EXPECT_TRUE(stmt->items[1].has_alias);
  EXPECT_EQ(stmt->items[1].alias, "revenue");
  EXPECT_EQ(stmt->items[1].expr->ToString(), "(sum (* l_extendedprice (- 1 l_discount)))");
  ASSERT_EQ(stmt->from.size(), 3u);
  EXPECT_EQ(stmt->from[2]->table, "lineitem");
  ASSERT_EQ(stmt->group_by.size(), 3u);
  ASSERT_EQ(stmt->order_by.size(), 2u);
  EXPECT_FALSE(stmt->order_by[0].ascending);
  EXPECT_TRUE(stmt->order_by[1].ascending);
  ASSERT_TRUE(stmt->limit.has_value());
  EXPECT_EQ(*stmt->limit, 10);
  ASSERT_NE(stmt->where, nullptr);
  EXPECT_EQ(stmt->where->ToString().rfind("(AND (AND (AND (AND (= c_mktsegment 'BUILDING')", 0), 0u)
      << stmt->where->ToString();
}

TEST(ParserTest, Q6HasDateIntervalAndBetween) {
  const auto stmt = MustParse(ReadFile("tests/tpch/queries/q6.sql"));
  ASSERT_NE(stmt, nullptr);
  const std::string where = stmt->where->ToString();
  EXPECT_NE(where.find("(< l_shipdate (+ (date '1994-01-01') (interval 1 year)))"),
            std::string::npos)
      << where;
  EXPECT_NE(where.find("(between l_discount (- 0.06 0.01) (+ 0.06 0.01))"), std::string::npos)
      << where;
}

TEST(ParserTest, HavingCountStarIsNotNullIn) {
  const auto stmt = MustParse(
      "select a, count(*) as n from t where b is not null and c in (1, 2, 3) and d not in ('x') "
      "group by a having count(*) > 1");
  ASSERT_NE(stmt, nullptr);
  EXPECT_EQ(stmt->items[1].expr->ToString(), "(count *)");
  EXPECT_EQ(stmt->items[1].alias, "n");
  EXPECT_EQ(stmt->where->ToString(), "(AND (AND (is-not-null b) (in c 1 2 3)) (not-in d 'x'))");
  ASSERT_NE(stmt->having, nullptr);
  EXPECT_EQ(stmt->having->ToString(), "(> (count *) 1)");
}

TEST(ParserTest, ExplicitJoinOn) {
  const auto stmt =
      MustParse("select 1 from a join b on a.x = b.x inner join c as cc on b.y = cc.y, d");
  ASSERT_NE(stmt, nullptr);
  ASSERT_EQ(stmt->from.size(), 2u);
  const ast::FromItem& top = *stmt->from[0];
  ASSERT_EQ(top.kind, ast::FromItem::Kind::kJoin);
  EXPECT_EQ(top.on->ToString(), "(= b.y cc.y)");
  EXPECT_EQ(top.right->table, "c");
  EXPECT_EQ(top.right->alias, "cc");
  ASSERT_EQ(top.left->kind, ast::FromItem::Kind::kJoin);
  EXPECT_EQ(top.left->on->ToString(), "(= a.x b.x)");
  EXPECT_EQ(top.left->left->table, "a");
  EXPECT_EQ(stmt->from[1]->table, "d");
}

TEST(ParserTest, OrderByDirectionAndNulls) {
  const auto stmt =
      MustParse("select x, y from t order by x desc nulls first, y, z asc nulls last");
  ASSERT_NE(stmt, nullptr);
  ASSERT_EQ(stmt->order_by.size(), 3u);
  EXPECT_FALSE(stmt->order_by[0].ascending);
  EXPECT_EQ(stmt->order_by[0].nulls, ast::NullsOrder::kFirst);
  EXPECT_TRUE(stmt->order_by[1].ascending);
  EXPECT_EQ(stmt->order_by[1].nulls, ast::NullsOrder::kDefault);
  EXPECT_TRUE(stmt->order_by[2].ascending);
  EXPECT_EQ(stmt->order_by[2].nulls, ast::NullsOrder::kLast);
}

TEST(ParserTest, AliasesWithAndWithoutAs) {
  const auto stmt = MustParse("select a as x, b y, c \"Z\" from lineitem l, orders as o");
  ASSERT_NE(stmt, nullptr);
  EXPECT_EQ(stmt->items[0].alias, "x");
  EXPECT_EQ(stmt->items[1].alias, "y");
  EXPECT_EQ(stmt->items[2].alias, "Z");
  EXPECT_TRUE(stmt->items[2].alias_quoted);
  EXPECT_FALSE(stmt->items[2].expr == nullptr);
  EXPECT_EQ(stmt->from[0]->alias, "l");
  EXPECT_EQ(stmt->from[1]->alias, "o");
  EXPECT_EQ(stmt->items[0].source_text, "a");
}

TEST(ParserTest, SourceTextOfAnItemIsTheSqlItWasWrittenAs) {
  const auto stmt = MustParse("select sum(a  *  b), count(*) from t");
  ASSERT_NE(stmt, nullptr);
  EXPECT_EQ(stmt->items[0].source_text, "sum(a  *  b)");
  EXPECT_EQ(stmt->items[1].source_text, "count(*)");
}

TEST(ParserTest, IdentifiersAreLowerCasedUnlessQuoted) {
  EXPECT_EQ(ExprOf("L_Quantity"), "l_quantity");
  EXPECT_EQ(ExprOf("\"L_Quantity\""), "\"L_Quantity\"");
  EXPECT_EQ(ExprOf("LineItem.L_Quantity"), "lineitem.l_quantity");
}

TEST(ParserTest, PrecedenceOfArithmetic) {
  EXPECT_EQ(ExprOf("a + b * c"), "(+ a (* b c))");
  EXPECT_EQ(ExprOf("a * b + c"), "(+ (* a b) c)");
  EXPECT_EQ(ExprOf("a - b - c"), "(- (- a b) c)");
  EXPECT_EQ(ExprOf("(a + b) * c"), "(* (+ a b) c)");
  EXPECT_EQ(ExprOf("-a * b"), "(* (neg a) b)");
  EXPECT_EQ(ExprOf("- - a"), "(neg (neg a))");
  EXPECT_EQ(ExprOf("a + b < c * d"), "(< (+ a b) (* c d))");
}

TEST(ParserTest, PrecedenceOfLogic) {
  EXPECT_EQ(ExprOf("NOT a = b AND c"), "(AND (not (= a b)) c)");
  EXPECT_EQ(ExprOf("a OR b AND c"), "(OR a (AND b c))");
  EXPECT_EQ(ExprOf("a AND b OR c"), "(OR (AND a b) c)");
  EXPECT_EQ(ExprOf("NOT NOT a"), "(not (not a))");
  EXPECT_EQ(ExprOf("a = 1 and b between 2 and 3 or c"), "(OR (AND (= a 1) (between b 2 3)) c)");
  EXPECT_EQ(ExprOf("a not between 1 + 1 and 3"), "(not-between a (+ 1 1) 3)");
  EXPECT_EQ(ExprOf("a is null and b"), "(AND (is-null a) b)");
  EXPECT_EQ(ExprOf("a + 1 is not null"), "(is-not-null (+ a 1))");
}

TEST(ParserTest, LiteralForms) {
  EXPECT_EQ(ExprOf("date '1998-12-01' - interval '90' day"),
            "(- (date '1998-12-01') (interval 90 day))");
  EXPECT_EQ(ExprOf("interval '-3' month"), "(interval -3 month)");
  EXPECT_EQ(ExprOf("'it''s'"), "'it's'");
  EXPECT_EQ(ExprOf("null"), "null");
  EXPECT_EQ(ExprOf("true and FALSE"), "(AND true false)");
  EXPECT_EQ(ExprOf(".5 + 12"), "(+ .5 12)");
  EXPECT_EQ(ExprOf("a <> b"), "(<> a b)");
  EXPECT_EQ(ExprOf("a != b"), "(<> a b)");
}

TEST(ParserTest, DateAndIntervalCanStillBeColumnNames) {
  EXPECT_EQ(ExprOf("date + interval"), "(+ date interval)");
}

TEST(ParserTest, SemicolonAndCommentsAreAccepted) {
  EXPECT_NE(MustParse("select 1 from t;"), nullptr);
  EXPECT_NE(MustParse("select 1 -- c\nfrom t -- d\n;"), nullptr);
}

struct BadCase {
  const char* sql;
  const char* prefix;
};

TEST(ParserTest, SyntaxErrorsPointAtTheOffendingToken) {
  const BadCase cases[] = {
      {"select from t", "syntax error at 1:8: "},
      {"select a b c from t", "syntax error at 1:12: "},
      {"select a from", "syntax error at 1:14: "},
      {"select a from t where", "syntax error at 1:22: "},
      {"select a from t group a", "syntax error at 1:23: "},
      {"select (a + 1 from t", "syntax error at 1:15: "},
      {"select a from t limit x", "syntax error at 1:23: "},
      {"select a\nfrom t\nwhere a = = 1", "syntax error at 3:11: "},
      {"select a from t join u", "syntax error at 1:23: "},
      {"select a from t order by a nulls middle", "syntax error at 1:34: "},
      {"select a from t where a in ()", "syntax error at 1:29: "},
      {"select 1 +", "syntax error at 1:11: "},
      {"select a from t t2 t3", "syntax error at 1:20: "},
      {"select a from t; select b from t", "syntax error at 1:18: "},
  };
  for (const BadCase& c : cases) {
    const ParseResult result = Parse(c.sql);
    EXPECT_FALSE(result.ok()) << c.sql;
    EXPECT_EQ(result.error.rfind(c.prefix, 0), 0u) << c.sql << "\n  got: " << result.error;
  }
}

TEST(ParserTest, UnsupportedFeaturesGetTheirOwnMessage) {
  struct Case {
    const char* sql;
    const char* needle;
  };
  const Case cases[] = {
      {"select a / b from t", "division is not supported"},
      {"select a from t left join u on 1 = 1", "LEFT is not supported"},
      {"select distinct a from t", "DISTINCT is not supported"},
      {"select count(distinct a) from t", "DISTINCT is not supported"},
      {"select a from t where a like 'x'", "LIKE is not supported"},
      {"select (select 1) from t", "subqueries are not supported"},
      {"select * from t", "SELECT * is not supported"},
      {"select a from t union select a from u", "UNION is not supported"},
      {"select a from t limit 1 offset 2", "OFFSET is not supported"},
  };
  for (const Case& c : cases) {
    const ParseResult result = Parse(c.sql);
    EXPECT_FALSE(result.ok()) << c.sql;
    EXPECT_NE(result.error.find(c.needle), std::string::npos)
        << c.sql << "\n  got: " << result.error;
    EXPECT_EQ(result.error.rfind("syntax error at ", 0), 0u) << result.error;
  }
}

TEST(ParserTest, DivisionErrorPointsAtTheSlash) {
  const ParseResult result = Parse("select a / b from t");
  EXPECT_EQ(result.error.rfind("syntax error at 1:10: division is not supported", 0), 0u)
      << result.error;
}

TEST(ParserTest, DeepNestingIsASyntaxErrorNotACrash) {
  const std::string open(10000, '(');
  const std::string close(10000, ')');
  const ParseResult nested = Parse("select " + open + "1" + close + " from t");
  EXPECT_FALSE(nested.ok());
  EXPECT_EQ(nested.error.rfind("syntax error at ", 0), 0u);
  EXPECT_NE(nested.error.find("nesting too deep"), std::string::npos) << nested.error;

  std::string nots = "select ";
  for (int i = 0; i < 10000; ++i) nots += "not ";
  const ParseResult not_chain = Parse(nots + "a from t");
  EXPECT_NE(not_chain.error.find("nesting too deep"), std::string::npos) << not_chain.error;

  const ParseResult negs = Parse("select " + MinusChain(10000) + "1 from t");
  EXPECT_NE(negs.error.find("nesting too deep"), std::string::npos) << negs.error;
}

TEST(ParserTest, NestingExactlyAtTheLimitParses) {
  const std::string ok =
      "select " + std::string(200, '(') + "1" + std::string(200, ')') + " from t";
  EXPECT_TRUE(Parse(ok).ok());
}

TEST(ParserTest, OneMegabyteOfPlusIsASyntaxErrorNotACrash) {
  std::string sql = "SELECT 1";
  while (sql.size() < (1u << 20)) sql += "+1";
  sql += " FROM t";
  const ParseResult result = Parse(sql);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.error.rfind("syntax error at ", 0), 0u);
  EXPECT_NE(result.error.find("nesting too deep"), std::string::npos) << result.error;
}

TEST(ParserTest, DeepJoinChainAndLongAndChainAreRefused) {
  std::string joins = "select 1 from t0";
  for (int i = 1; i < 2000; ++i) {
    joins += " join t" + std::to_string(i) + " on t0.x = t" + std::to_string(i) + ".x";
  }
  const ParseResult j = Parse(joins);
  EXPECT_FALSE(j.ok());
  EXPECT_NE(j.error.find("nesting too deep"), std::string::npos) << j.error;

  std::string ands = "select 1 from t where a = 1";
  for (int i = 0; i < 2000; ++i) ands += " and a = 1";
  const ParseResult a = Parse(ands);
  EXPECT_FALSE(a.ok());
  EXPECT_NE(a.error.find("nesting too deep"), std::string::npos) << a.error;
}

TEST(ParserTest, ManyItemsAreFine) {
  std::string sql = "select 1";
  for (int i = 0; i < 5000; ++i) sql += ", " + std::to_string(i);
  sql += " from t";
  EXPECT_TRUE(Parse(sql).ok());
}

TEST(ParserTest, EmptyAndGarbageInputs) {
  EXPECT_FALSE(Parse("").ok());
  EXPECT_FALSE(Parse("   ").ok());
  EXPECT_FALSE(Parse(")").ok());
  EXPECT_FALSE(Parse(std::string("\0\0", 2)).ok());
  EXPECT_EQ(Parse("").error.rfind("syntax error at 1:1: ", 0), 0u);
}

}  // namespace
}  // namespace quarry
