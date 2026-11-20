#include "binder/binder.h"

#include <arrow/scalar.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "binder/catalog.h"
#include "plan/logical.h"
#include "support/parquet_fixture.h"
#include "types/date.h"

namespace quarry {
namespace {

using testing::Int32Column;
using testing::Int64Column;
using testing::TempDir;
using testing::WriteParquet;

const Catalog& Tpch() {
  static const Catalog* catalog = [] {
    auto* c = new Catalog();
    EXPECT_TRUE(c->AddDirectory("tests/data/tpch-sf0.01").ok());
    return c;
  }();
  return *catalog;
}

LogicalPlan MustBind(const std::string& sql, const Catalog& catalog = Tpch()) {
  auto plan = ParseAndBind(sql, catalog);
  EXPECT_TRUE(plan.ok()) << sql << "\n" << plan.status().ToString();
  if (!plan.ok()) return nullptr;
  return std::move(*plan);
}

std::string ErrorOf(const std::string& sql, const Catalog& catalog = Tpch()) {
  auto plan = ParseAndBind(sql, catalog);
  EXPECT_FALSE(plan.ok()) << sql;
  return plan.ok() ? "" : plan.status().message();
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path);
  EXPECT_TRUE(in.good()) << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

template <typename T>
const T& As(const LogicalPlan& node, LogicalKind kind) {
  EXPECT_EQ(node->kind, kind);
  return static_cast<const T&>(*node);
}

// The Project at the top of `SELECT ... FROM ...` (no ORDER BY, no LIMIT).
const LogicalProject& TopProject(const LogicalPlan& plan) {
  return As<LogicalProject>(plan, LogicalKind::kProject);
}

TEST(BinderTest, UnquotedIdentifiersAreCaseInsensitive) {
  const auto plan = MustBind("SELECT L_QUANTITY FROM LineItem");
  ASSERT_NE(plan, nullptr);
  const LogicalProject& project = TopProject(plan);
  ASSERT_EQ(project.exprs.size(), 1u);
  EXPECT_EQ(project.names[0], "l_quantity");
  EXPECT_EQ(project.exprs[0]->type.ToString(), "DECIMAL(15,2)");
  const auto types = plan->output_types();
  ASSERT_EQ(types.size(), 1u);
  EXPECT_EQ(types[0].ToString(), "DECIMAL(15,2)");
  ASSERT_EQ(project.child->kind, LogicalKind::kScan);
  const auto& scan = static_cast<const LogicalScan&>(*project.child);
  EXPECT_EQ(scan.table_name, "lineitem");
  EXPECT_NE(scan.table, nullptr);
  EXPECT_EQ(scan.columns.size(), 16u);
}

TEST(BinderTest, QuotedIdentifiersAreExact) {
  const std::string message = ErrorOf("SELECT \"L_QUANTITY\" FROM lineitem");
  EXPECT_EQ(message.rfind("binder error at 1:8: ", 0), 0u) << message;
  EXPECT_TRUE(Contains(message, "unknown column")) << message;
  EXPECT_TRUE(Contains(message, "L_QUANTITY")) << message;
  EXPECT_NE(MustBind("SELECT \"l_quantity\" FROM lineitem"), nullptr);
  const std::string table = ErrorOf("SELECT 1 FROM \"LineItem\"");
  EXPECT_TRUE(Contains(table, "unknown table")) << table;
  EXPECT_EQ(table.rfind("binder error at 1:15: ", 0), 0u) << table;
}

TEST(BinderTest, UnknownTableColumnAndQualifier) {
  EXPECT_TRUE(Contains(ErrorOf("select a from nope"), "unknown table nope"));
  EXPECT_TRUE(Contains(ErrorOf("select nope from region"), "unknown column nope"));
  EXPECT_TRUE(Contains(ErrorOf("select x.r_name from region"), "unknown table or alias x"));
  EXPECT_TRUE(Contains(ErrorOf("select region.nope from region"), "unknown column region.nope"));
  // An alias hides the table's own name.
  EXPECT_TRUE(Contains(ErrorOf("select region.r_name from region r"), "unknown table or alias"));
  EXPECT_NE(MustBind("select r.r_name from region r"), nullptr);
}

TEST(BinderTest, AmbiguousColumnListsBothCandidates) {
  TempDir dir;
  Catalog catalog;
  ASSERT_TRUE(
      catalog
          .AddTable("lineitem", {WriteParquet(dir.path() / "a.parquet", {Int32Column("x", {1})})})
          .ok());
  ASSERT_TRUE(
      catalog.AddTable("orders", {WriteParquet(dir.path() / "b.parquet", {Int32Column("x", {1})})})
          .ok());
  const std::string message = ErrorOf("select x from lineitem, orders", catalog);
  EXPECT_EQ(message.rfind("binder error at 1:8: ", 0), 0u) << message;
  EXPECT_TRUE(Contains(message, "ambiguous")) << message;
  EXPECT_TRUE(Contains(message, "lineitem.x, orders.x")) << message;
  EXPECT_NE(MustBind("select lineitem.x, orders.x from lineitem, orders", catalog), nullptr);

  // The same table twice needs distinct aliases, and then both are candidates.
  const std::string self = ErrorOf("select x from lineitem a, lineitem b", catalog);
  EXPECT_TRUE(Contains(self, "a.x, b.x")) << self;
  const std::string dup = ErrorOf("select 1 from lineitem, lineitem", catalog);
  EXPECT_TRUE(Contains(dup, "more than once")) << dup;
}

TEST(BinderTest, AggregateInWhereIsAnError) {
  const std::string message = ErrorOf("select l_orderkey from lineitem where sum(l_quantity) > 1");
  EXPECT_EQ(message.rfind("binder error at 1:", 0), 0u) << message;
  EXPECT_TRUE(Contains(message, "aggregate")) << message;
  EXPECT_TRUE(Contains(message, "WHERE")) << message;
  EXPECT_TRUE(
      Contains(ErrorOf("select l_orderkey from lineitem group by sum(l_quantity)"), "GROUP BY"));
  EXPECT_TRUE(Contains(ErrorOf("select sum(sum(l_quantity)) from lineitem"), "nested"));
  EXPECT_TRUE(Contains(ErrorOf("select 1 from orders join lineitem on count(*) = 1"), "ON"));
}

TEST(BinderTest, ColumnOutsideGroupByIsAnError) {
  const std::string message =
      ErrorOf("select l_returnflag, sum(l_quantity) from lineitem group by l_linestatus");
  EXPECT_EQ(message.rfind("binder error at 1:8: ", 0), 0u) << message;
  EXPECT_TRUE(Contains(message, "l_returnflag")) << message;
  EXPECT_TRUE(Contains(message, "GROUP BY")) << message;
  EXPECT_TRUE(Contains(ErrorOf("select l_returnflag, count(*) from lineitem"), "GROUP BY"));
  EXPECT_TRUE(
      Contains(ErrorOf("select l_returnflag from lineitem group by l_linestatus"), "GROUP BY"));
  EXPECT_TRUE(Contains(ErrorOf("select sum(l_quantity) from lineitem having l_returnflag = 'A'"),
                       "GROUP BY"));
  EXPECT_NE(MustBind("select l_returnflag, sum(l_quantity) from lineitem group by l_returnflag"),
            nullptr);
}

TEST(BinderTest, GroupByExpressionsMatchStructurally) {
  EXPECT_NE(MustBind("select l_quantity + 1, count(*) from lineitem group by l_quantity + 1"),
            nullptr);
  EXPECT_NE(MustBind("select (l_quantity + 1) * 2 from lineitem group by l_quantity + 1"), nullptr);
  EXPECT_NE(MustBind("select l_quantity + 1 from lineitem group by l_quantity"), nullptr);
  EXPECT_FALSE(
      ParseAndBind("select l_quantity from lineitem group by l_quantity + 1", Tpch()).ok());
}

TEST(BinderTest, OrderByAliasOrdinalAndHiddenColumns) {
  const auto plan = MustBind(
      "select l_returnflag, sum(l_extendedprice) as revenue from lineitem group by l_returnflag "
      "order by revenue desc");
  ASSERT_NE(plan, nullptr);
  const auto& sort = As<LogicalSort>(plan, LogicalKind::kSort);
  ASSERT_EQ(sort.keys.size(), 1u);
  EXPECT_FALSE(sort.keys[0].ascending);
  EXPECT_FALSE(sort.keys[0].nulls_first);
  EXPECT_EQ(sort.keys[0].expr->ToString(), "#1");
  EXPECT_EQ(sort.child->kind, LogicalKind::kProject);

  const std::string ordinal = ErrorOf("select l_returnflag from lineitem order by 1");
  EXPECT_EQ(ordinal.rfind("binder error at 1:", 0), 0u) << ordinal;
  EXPECT_TRUE(Contains(ordinal, "ordinals are not supported")) << ordinal;
  EXPECT_TRUE(Contains(ErrorOf("select l_returnflag from lineitem group by 1"),
                       "ordinals are not supported"));

  // A sort key that is not in the select list is computed, sorted on, and dropped again.
  const auto hidden = MustBind("select l_orderkey from lineitem order by l_quantity");
  ASSERT_NE(hidden, nullptr);
  const auto& top = TopProject(hidden);
  EXPECT_EQ(top.exprs.size(), 1u);
  const auto& sort2 = As<LogicalSort>(top.child, LogicalKind::kSort);
  EXPECT_EQ(sort2.child->output_types().size(), 2u);
  EXPECT_EQ(hidden->output_types().size(), 1u);
}

TEST(BinderTest, SortKeysRecordNullsFirstExplicitly) {
  const auto plan = MustBind(
      "select r_regionkey, r_name from region order by r_regionkey desc nulls first, r_name, "
      "r_comment asc nulls last, r_regionkey desc");
  ASSERT_NE(plan, nullptr);
  const auto& sort = As<LogicalSort>(
      plan->kind == LogicalKind::kProject ? static_cast<const LogicalProject&>(*plan).child : plan,
      LogicalKind::kSort);
  ASSERT_EQ(sort.keys.size(), 4u);
  EXPECT_FALSE(sort.keys[0].ascending);
  EXPECT_TRUE(sort.keys[0].nulls_first);
  EXPECT_TRUE(sort.keys[1].ascending);
  EXPECT_FALSE(sort.keys[1].nulls_first);
  EXPECT_FALSE(sort.keys[2].nulls_first);
  EXPECT_FALSE(sort.keys[3].ascending);
  EXPECT_FALSE(sort.keys[3].nulls_first) << "NULLS LAST is the default for DESC too";
}

TEST(BinderTest, AmbiguousOrderByAliasIsAnError) {
  EXPECT_TRUE(Contains(ErrorOf("select r_regionkey as k, r_name as k from region order by k"),
                       "ambiguous"));
}

TEST(BinderTest, AliasesAreNotVisibleInWhere) {
  EXPECT_TRUE(
      Contains(ErrorOf("select r_regionkey as k from region where k > 1"), "unknown column k"));
}

TEST(BinderTest, DateIntervalArithmeticFoldsToADateLiteral) {
  const auto plan = MustBind("select date '1998-12-01' - interval '90' day from region");
  ASSERT_NE(plan, nullptr);
  const BoundExpr& e = *TopProject(plan).exprs[0];
  ASSERT_EQ(e.kind, ExprKind::kLiteral);
  EXPECT_EQ(e.type.ToString(), "DATE");
  const auto* date = dynamic_cast<const arrow::Date32Scalar*>(e.literal.get());
  ASSERT_NE(date, nullptr);
  EXPECT_EQ(date->value, *ParseDate("1998-09-02"));

  const auto year = MustBind("select date '1994-01-01' + interval '1' year from region");
  const auto* y =
      dynamic_cast<const arrow::Date32Scalar*>(TopProject(year).exprs[0]->literal.get());
  ASSERT_NE(y, nullptr);
  EXPECT_EQ(y->value, *ParseDate("1995-01-01"));

  // A month step clamps to the last day of the month, like DuckDB.
  const auto month = MustBind("select date '1994-01-31' + interval '1' month from region");
  const auto* m =
      dynamic_cast<const arrow::Date32Scalar*>(TopProject(month).exprs[0]->literal.get());
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(m->value, *ParseDate("1994-02-28"));

  const auto chain =
      MustBind("select date '1994-01-01' + interval '1' day - interval '2' day from region");
  const auto* c =
      dynamic_cast<const arrow::Date32Scalar*>(TopProject(chain).exprs[0]->literal.get());
  ASSERT_NE(c, nullptr);
  EXPECT_EQ(c->value, *ParseDate("1993-12-31"));
}

TEST(BinderTest, IntervalNeedsAConstantDateOnTheLeft) {
  EXPECT_TRUE(
      Contains(ErrorOf("select l_shipdate + interval '1' day from lineitem"), "constant DATE"));
  EXPECT_TRUE(Contains(ErrorOf("select interval '1' day from region"), "INTERVAL"));
  EXPECT_TRUE(
      Contains(ErrorOf("select interval '1' day + date '1994-01-01' from region"), "INTERVAL"));
  EXPECT_TRUE(Contains(ErrorOf("select date '1994-13-01' from region"), "date"));
  EXPECT_TRUE(
      Contains(ErrorOf("select date '9999-12-31' + interval '1' day from region"), "binder error"));
}

TEST(BinderTest, DivisionIsASyntaxError) {
  const std::string message = ErrorOf("select l_quantity / 2 from lineitem");
  EXPECT_EQ(message.rfind("syntax error at 1:19: ", 0), 0u) << message;
  EXPECT_TRUE(Contains(message, "division is not supported")) << message;
}

TEST(BinderTest, LiteralsAndTheirTypes) {
  const auto plan = MustBind(
      "select 1, 3000000000, 0.05, 'a', true, null, -5, -0.5, date '1994-01-01' from region");
  ASSERT_NE(plan, nullptr);
  const auto types = plan->output_types();
  ASSERT_EQ(types.size(), 9u);
  EXPECT_EQ(types[0].ToString(), "INTEGER");
  EXPECT_EQ(types[1].ToString(), "BIGINT");
  EXPECT_EQ(types[2].ToString(), "DECIMAL(3,2)");
  EXPECT_EQ(types[3].ToString(), "VARCHAR");
  EXPECT_EQ(types[4].ToString(), "BOOLEAN");
  EXPECT_EQ(types[5].ToString(), "INTEGER");
  EXPECT_EQ(types[6].ToString(), "INTEGER");
  EXPECT_EQ(types[7].ToString(), "DECIMAL(2,1)");
  EXPECT_EQ(types[8].ToString(), "DATE");
  EXPECT_TRUE(Contains(ErrorOf("select 99999999999999999999 from region"), "out of range"));
}

TEST(BinderTest, NullTakesTheTypeOfTheOtherOperand) {
  EXPECT_NE(MustBind("select 1 from nation where n_name = null"), nullptr);
  EXPECT_NE(MustBind("select 1 from nation where null = n_nationkey"), nullptr);
  EXPECT_NE(MustBind("select 1 from nation where n_nationkey between null and 5"), nullptr);
  EXPECT_NE(MustBind("select 1 from nation where n_nationkey in (1, null)"), nullptr);
}

TEST(BinderTest, PredicateForms) {
  const auto plan = MustBind(
      "select n_name from nation where n_nationkey in (1, 2, -3) and n_regionkey between 1 and 2 "
      "and n_comment is not null and not n_name = 'x' or n_nationkey not in (4)");
  ASSERT_NE(plan, nullptr);
  const auto& filter = As<LogicalFilter>(TopProject(plan).child, LogicalKind::kFilter);
  EXPECT_EQ(filter.predicate->type.ToString(), "BOOLEAN");
  EXPECT_TRUE(Contains(filter.predicate->ToString(), "IN"));
  EXPECT_TRUE(
      Contains(ErrorOf("select 1 from nation where n_nationkey in (n_regionkey)"), "constant"));
  EXPECT_TRUE(Contains(ErrorOf("select 1 from nation where n_name in (1)"), "binder error"));
}

TEST(BinderTest, TypeErrorsCarryAPosition) {
  const std::string compare = ErrorOf("select 1 from lineitem where l_shipdate = 5");
  EXPECT_EQ(compare.rfind("binder error at 1:", 0), 0u) << compare;
  const std::string where = ErrorOf("select 1 from lineitem where l_quantity");
  EXPECT_TRUE(Contains(where, "WHERE must be a BOOLEAN")) << where;
  EXPECT_EQ(where.rfind("binder error at 1:30: ", 0), 0u) << where;
  EXPECT_TRUE(Contains(ErrorOf("select l_comment + 1 from lineitem"), "binder error"));
  EXPECT_TRUE(Contains(ErrorOf("select sum(l_comment) from lineitem"), "binder error"));
  EXPECT_TRUE(Contains(ErrorOf("select foo(l_quantity) from lineitem"), "unknown function foo"));
  EXPECT_TRUE(Contains(ErrorOf("select sum(*) from lineitem"), "binder error"));
  EXPECT_TRUE(Contains(ErrorOf("select sum(l_quantity, l_tax) from lineitem"), "one argument"));
  EXPECT_TRUE(Contains(ErrorOf("select 1 from lineitem group by l_tax having sum(l_quantity)"),
                       "HAVING must be a BOOLEAN"));
  const std::string second_line = ErrorOf("select 1\nfrom lineitem\nwhere nope = 1");
  EXPECT_EQ(second_line.rfind("binder error at 3:7: ", 0), 0u) << second_line;
}

TEST(BinderTest, SelectListNames) {
  const auto plan = MustBind(
      "select l_quantity, l_quantity as q, l_quantity * 2, sum(l_tax), count(*) from lineitem "
      "group by l_quantity");
  ASSERT_NE(plan, nullptr);
  const auto& names = TopProject(plan).names;
  ASSERT_EQ(names.size(), 5u);
  EXPECT_EQ(names[0], "l_quantity");
  EXPECT_EQ(names[1], "q");
  EXPECT_EQ(names[2], "l_quantity * 2");
  EXPECT_EQ(names[3], "sum(l_tax)");
  EXPECT_EQ(names[4], "count(*)");
}

TEST(BinderTest, AggregateTypesAndDeduplication) {
  const auto plan = MustBind(
      "select sum(l_quantity), sum(l_quantity), avg(l_quantity), count(*), count(l_comment), "
      "min(l_shipdate), max(l_comment), sum(l_orderkey) from lineitem");
  ASSERT_NE(plan, nullptr);
  const auto& agg = As<LogicalAggregate>(TopProject(plan).child, LogicalKind::kAggregate);
  EXPECT_TRUE(agg.groups.empty());
  EXPECT_EQ(agg.aggregates.size(), 7u) << "the repeated SUM(l_quantity) is computed once";
  const auto types = plan->output_types();
  ASSERT_EQ(types.size(), 8u);
  EXPECT_EQ(types[0].ToString(), "DECIMAL(38,2)");
  EXPECT_EQ(types[2].ToString(), "DOUBLE");
  EXPECT_EQ(types[3].ToString(), "BIGINT");
  EXPECT_EQ(types[5].ToString(), "DATE");
  EXPECT_EQ(types[6].ToString(), "VARCHAR");
  EXPECT_EQ(types[7].ToString(), "DECIMAL(38,0)");
}

TEST(BinderTest, HavingFiltersTheAggregate) {
  const auto plan = MustBind(
      "select l_returnflag, count(*) as n from lineitem group by l_returnflag having count(*) > 1 "
      "and sum(l_quantity) > 0");
  ASSERT_NE(plan, nullptr);
  const auto& filter = As<LogicalFilter>(TopProject(plan).child, LogicalKind::kFilter);
  const auto& agg = As<LogicalAggregate>(filter.child, LogicalKind::kAggregate);
  EXPECT_EQ(agg.groups.size(), 1u);
  EXPECT_EQ(agg.aggregates.size(), 2u);
}

TEST(BinderTest, CommaJoinIsACrossProductUnderAFilter) {
  const auto plan = MustBind("select n_name from nation, region where n_regionkey = r_regionkey");
  ASSERT_NE(plan, nullptr);
  const auto& filter = As<LogicalFilter>(TopProject(plan).child, LogicalKind::kFilter);
  const auto& cross = As<LogicalCrossProduct>(filter.child, LogicalKind::kCrossProduct);
  ASSERT_EQ(cross.children.size(), 2u);
  EXPECT_EQ(cross.children[0]->kind, LogicalKind::kScan);
  // The predicate indexes the concatenated output: nation has 4 columns, region 3.
  EXPECT_EQ(cross.output_types().size(), 7u);
  EXPECT_TRUE(Contains(filter.predicate->ToString(), "#2"));
  EXPECT_TRUE(Contains(filter.predicate->ToString(), "#4"));
}

TEST(BinderTest, ExplicitJoinSplitsKeysFromTheResidual) {
  const auto plan = MustBind(
      "select o_orderkey from orders join customer on c_custkey = o_custkey and o_totalprice > "
      "c_acctbal");
  ASSERT_NE(plan, nullptr);
  const auto& join = As<LogicalJoin>(TopProject(plan).child, LogicalKind::kJoin);
  ASSERT_EQ(join.left_keys.size(), 1u);
  ASSERT_EQ(join.right_keys.size(), 1u);
  // The key written customer-first is swapped so that each key indexes its own side's output.
  EXPECT_EQ(join.left->kind, LogicalKind::kScan);
  EXPECT_EQ(static_cast<const LogicalScan&>(*join.left).table_name, "orders");
  EXPECT_EQ(join.left_keys[0]->ToString(), "#1");
  EXPECT_EQ(join.right_keys[0]->ToString(), "#0");
  EXPECT_EQ(join.left_keys[0]->type, join.right_keys[0]->type);
  ASSERT_NE(join.residual, nullptr);
  EXPECT_TRUE(Contains(join.residual->ToString(), ">"));
  EXPECT_EQ(join.output_types().size(), 9u + 8u);
}

TEST(BinderTest, JoinKeysOfDifferentTypesAreCastToOne) {
  TempDir dir;
  Catalog catalog;
  ASSERT_TRUE(
      catalog.AddTable("a", {WriteParquet(dir.path() / "a.parquet", {Int32Column("x", {1})})})
          .ok());
  ASSERT_TRUE(
      catalog.AddTable("b", {WriteParquet(dir.path() / "b.parquet", {Int64Column("y", {1})})})
          .ok());
  const auto plan = MustBind("select 1 from a join b on x = y", catalog);
  ASSERT_NE(plan, nullptr);
  const auto& join = As<LogicalJoin>(TopProject(plan).child, LogicalKind::kJoin);
  ASSERT_EQ(join.left_keys.size(), 1u);
  EXPECT_EQ(join.left_keys[0]->type.ToString(), "BIGINT");
  EXPECT_EQ(join.right_keys[0]->type.ToString(), "BIGINT");
  EXPECT_EQ(join.residual, nullptr);
}

TEST(BinderTest, OnClauseSeesOnlyItsOwnJoin) {
  EXPECT_TRUE(
      Contains(ErrorOf("select 1 from region, nation join customer on c_nationkey = r_regionkey"),
               "unknown column r_regionkey"));
  EXPECT_NE(MustBind("select 1 from region, nation join customer on c_nationkey = n_nationkey "
                     "where r_regionkey = n_regionkey"),
            nullptr);
}

TEST(BinderTest, LimitAndTheShapeOfThePlan) {
  const auto plan = MustBind("select r_name from region order by r_name limit 3");
  ASSERT_NE(plan, nullptr);
  const auto& limit = As<LogicalLimit>(plan, LogicalKind::kLimit);
  EXPECT_EQ(limit.n, 3);
  EXPECT_EQ(limit.child->kind, LogicalKind::kSort);
  EXPECT_NE(MustBind("select r_name from region limit 0"), nullptr);
}

TEST(BinderTest, PlanToStringIsReadable) {
  const auto plan =
      MustBind("select r_name from region where r_regionkey > 1 order by r_name limit 2");
  ASSERT_NE(plan, nullptr);
  const std::string text = plan->ToString();
  EXPECT_EQ(text.rfind("Limit 2\n  Sort [#0 ASC NULLS LAST]\n    Project [", 0), 0u) << text;
  EXPECT_TRUE(Contains(text, "\n      Filter (#0 > 1)\n        Scan region")) << text;
  EXPECT_TRUE(Contains(text, "Scan region [r_regionkey, r_name, r_comment]"));
}

// The four TPC-H queries of the test suite all bind, with the plan shapes later tasks rely on.
TEST(BinderTest, TpchQ1) {
  const auto plan = MustBind(ReadFile("tests/tpch/queries/q1.sql"));
  ASSERT_NE(plan, nullptr);
  const auto& sort = As<LogicalSort>(
      plan->kind == LogicalKind::kProject ? static_cast<const LogicalProject&>(*plan).child : plan,
      LogicalKind::kSort);
  EXPECT_EQ(sort.keys.size(), 2u);
  const auto types = plan->output_types();
  ASSERT_EQ(types.size(), 10u);
  EXPECT_EQ(types[0].ToString(), "VARCHAR");
  EXPECT_EQ(types[2].ToString(), "DECIMAL(38,2)");
  EXPECT_EQ(types[6].ToString(), "DOUBLE");
  EXPECT_EQ(types[9].ToString(), "BIGINT");
  EXPECT_TRUE(Contains(plan->ToString(), "Filter (#"));
}

TEST(BinderTest, TpchQ3) {
  const auto plan = MustBind(ReadFile("tests/tpch/queries/q3.sql"));
  ASSERT_NE(plan, nullptr);
  const auto& limit = As<LogicalLimit>(plan, LogicalKind::kLimit);
  EXPECT_EQ(limit.n, 10);
  const auto& sort = As<LogicalSort>(limit.child, LogicalKind::kSort);
  ASSERT_EQ(sort.keys.size(), 2u);
  EXPECT_FALSE(sort.keys[0].ascending);
  EXPECT_TRUE(sort.keys[1].ascending);
  EXPECT_TRUE(Contains(plan->ToString(), "CrossProduct"));
  ASSERT_EQ(plan->output_types().size(), 4u);
}

TEST(BinderTest, TpchQ5) {
  const auto plan = MustBind(ReadFile("tests/tpch/queries/q5.sql"));
  ASSERT_NE(plan, nullptr);
  const std::string text = plan->ToString();
  EXPECT_TRUE(Contains(text, "CrossProduct"));
  EXPECT_TRUE(Contains(text, "Scan region"));
  ASSERT_EQ(plan->output_types().size(), 2u);
  EXPECT_EQ(plan->output_types()[0].ToString(), "VARCHAR");
}

TEST(BinderTest, TpchQ6) {
  const auto plan = MustBind(ReadFile("tests/tpch/queries/q6.sql"));
  ASSERT_NE(plan, nullptr);
  const auto& agg = As<LogicalAggregate>(TopProject(plan).child, LogicalKind::kAggregate);
  EXPECT_TRUE(agg.groups.empty());
  ASSERT_EQ(agg.aggregates.size(), 1u);
  EXPECT_TRUE(agg.aggregates[0].result_type.is_decimal());
  // The folded date bounds are literals, not a runtime INTERVAL.
  const std::string text = plan->ToString();
  EXPECT_FALSE(Contains(text, "interval"));
}

}  // namespace
}  // namespace quarry
