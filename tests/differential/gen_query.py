"""Seeded random queries for the differential tests (spec section 5.4).

generate_queries(seed, dataset) returns 5 Query objects over the tables of gen_data.generate(seed).
Every query stays inside the SQL subset quarry documents in docs/sql.md, so that a difference from
DuckDB is a real bug:

  * no SELECT *, DISTINCT, division, LIKE, CASE, subqueries or outer joins;
  * every select item carries an alias (o0, o1, ... or g0.. and a0.. in aggregate queries), because
    the engines name an unaliased expression differently; ORDER BY names the alias, the one place
    quarry accepts one; GROUP BY and HAVING repeat the expression;
  * IN lists hold literals; DATE comparisons use DATE literals; DATE +- INTERVAL only on literals;
  * joins are inner joins on plain column equalities, always connected;
  * LIMIT only together with an ORDER BY over every output column, so the result is unique, and in
    an aggregate query the group keys come first in that ORDER BY;
  * multiplication only between values that cannot overflow (the "small" columns and literals), plus
    rare multiplications of edge columns, which the engines must agree to reject;
  * a DOUBLE aggregate sums only values whose sum has no rounding (k/1024 columns added or
    subtracted, multiplied by integer literals), so that the order of the additions cannot matter;
  * HAVING does not compare an AVG of a DECIMAL, which DuckDB computes to within one ulp only;
  * an overflow must not depend on which rows an engine happens to evaluate: where a filter or a join
    decides that, optimizers may legitimately differ (DuckDB evaluates the conjuncts of a WHERE one
    after another on the survivors). So outside a "plain" query (one table, no WHERE, where both
    engines evaluate every row) comparisons are only generated when the common type of the two sides
    fits 38 digits, edge columns take no part in arithmetic, and no DECIMAL(38) edge column is summed.

Each Query records the features it exercises; test_generator_coverage.py requires every one of
them often enough.
"""

import collections
import datetime
import decimal
import random
from dataclasses import dataclass, field

decimal.getcontext().prec = 80

QUERIES_PER_SEED = 5
BATCH_SIZES = [1, 7, 2048]
MEMORY_LIMITS = [None, "64KiB"]
RULES = ["fold_constants", "push_filters", "prune_row_groups", "prune_columns", "order_joins", "pick_build_side"]
COMPARISONS = {"=": "eq", "<>": "ne", "!=": "ne", "<": "lt", "<=": "le", ">": "gt", ">=": "ge"}
AGGREGATES = ["sum", "avg", "count", "count_star", "min", "max"]
INT64_MIN = -(2**63)


def all_features():
    """The names the coverage test requires."""
    names = []
    names += [f"pred:{p}" for p in ["eq", "ne", "lt", "le", "gt", "ge", "is_null", "is_not_null",
                                    "between", "not_between", "in", "not_in", "and", "or", "not"]]
    names += ["pred:cross_scale_decimal", "pred:wide_decimal", "pred:bool_column", "pred:in_with_null",
              "pred:string", "pred:date", "pred:date_interval", "pred:prunable"]
    names += [f"agg:{a}" for a in AGGREGATES]
    names += [f"join:{n}" for n in (1, 2, 3, 4)]
    names += ["join:self", "join:multi_key", "join:cycle", "join:comma", "join:on"]
    names += [f"group_by:{n}" for n in (1, 2, 3)]
    names += ["group_by:expression", "global_aggregate", "having", "order_by", "limit", "filter", "projection"]
    names += ["arith:add", "arith:sub", "arith:mul", "arith:neg", "arith:date_plus_int", "arith:edge"]
    names += ["data:empty_table", "data:single_row_table", "data:sorted_small_row_groups",
              "data:decimal38_near_1e37", "data:nulls"]
    names += [f"engine:batch:{b}" for b in BATCH_SIZES]
    names += ["engine:memory:unlimited", "engine:memory:64KiB"]
    names += ["engine:rule:none"] + [f"engine:rule:{r}" for r in RULES]
    return names


@dataclass
class Options:
    batch_size: int
    memory_limit: str | None
    disabled_rule: str | None

    def cli_args(self):
        args = ["--batch-size", str(self.batch_size)]
        if self.memory_limit:
            args += ["--memory-limit", self.memory_limit]
        if self.disabled_rule:
            args += ["--disable-rule", self.disabled_rule]
        return args

    def features(self):
        return [
            f"engine:batch:{self.batch_size}",
            f"engine:memory:{self.memory_limit or 'unlimited'}",
            f"engine:rule:{self.disabled_rule or 'none'}",
        ]


@dataclass
class Query:
    seed: int
    index: int
    sql: str
    count_sql: str  # the FROM and WHERE alone, counted to bound the work before a query runs
    ordered: bool
    approx: frozenset  # output columns compared with the AVG tolerance
    options: Options
    features: list = field(default_factory=list)
    # The FROM alone, counted when push_filters is off: the WHERE is then applied after the joins, so
    # the rows the joins make are the work, not the rows that survive the filter.
    from_count_sql: str = ""


@dataclass
class Expr:
    sql: str
    kind: str  # int, dec, dbl, date, str, bool
    small: bool = False  # cannot overflow when multiplied by another small value
    exact: bool = True  # a DOUBLE sum of it has no rounding (always true for other kinds)
    column: object = None  # the Column when the expression is a bare column
    scale: int | None = None  # for a bare DECIMAL column
    decimal: bool = False  # involves a DECIMAL column (AVG of it is only approximately reproducible)
    ps: tuple | None = None  # (precision, scale) of an integer or DECIMAL value; None for other kinds


def _column_ps(column):
    if column.kind == "dec":
        return column.precision, column.scale
    if column.kind == "int":
        return (10, 0) if column.sql_type == "INTEGER" else (19, 0)
    return None


class Builder:
    def __init__(self, rng, tables, hazard_ok=False):
        self.rng = rng
        self.tables = tables  # [(alias, Table)]
        self.hazard_ok = hazard_ok  # a plain query: overflow cannot depend on the plan
        self.table_of = dict(tables)
        self.features = collections.Counter()

    # ---- helpers --------------------------------------------------------------------------

    def feat(self, name):
        self.features[name] += 1

    def chance(self, p):
        return self.rng.random() < p

    def columns(self, kinds=None, roles=None):
        out = []
        for alias, table in self.tables:
            for column in table.columns:
                if kinds and column.kind not in kinds:
                    continue
                if roles and column.role not in roles:
                    continue
                out.append((alias, table, column))
        return out

    def column_expr(self, alias, column):
        return Expr(
            f"{alias}.{column.name}",
            column.kind,
            small=column.role in ("small", "key"),
            exact=True,
            column=column,
            scale=column.scale if column.kind == "dec" else None,
            decimal=column.kind == "dec",
            ps=_column_ps(column),
        )

    def pick_column(self, kinds=None, roles=None, weights=None):
        options = self.columns(kinds, roles)
        if not options:
            return None
        alias, table, column = self.rng.choice(options)
        return alias, table, column

    # ---- literals ---------------------------------------------------------------------------

    def sample_value(self, table, column):
        values = [v for v in column.values if v is not None]
        return self.rng.choice(values) if values else None

    def literal_sql(self, kind, value):
        if kind == "int":
            if value == INT64_MIN:
                value += 1
            return str(value) if value >= 0 else f"-{-value}"
        if kind == "dec":
            text = f"{abs(value):f}"
            if "." not in text:
                if len(text.lstrip("0")) >= 38:
                    return None  # an integer literal of 38 digits is not a number quarry reads
                text += ".0"
            return text if value >= 0 else f"-{text}"
        if kind == "dbl":
            text = f"{abs(decimal.Decimal(value)):f}"
            return text if (value > 0 or (value == 0 and str(value)[0] != "-")) else f"-{text}"
        if kind == "date":
            return f"DATE '{value.isoformat()}'"
        if kind == "str":
            return "'" + value.replace("'", "''") + "'"
        if kind == "bool":
            return "TRUE" if value else "FALSE"
        raise ValueError(kind)

    def random_literal(self, kind, table=None, column=None):
        """A literal of the kind: usually a value that occurs in the column, sometimes a made-up one."""
        rng = self.rng
        if column is not None and self.chance(0.7):
            value = self.sample_value(table, column)
            if value is not None:
                if kind == "dbl":
                    value = float(value)
                text = self.literal_sql(kind, value)
                if text is not None:
                    return text
        if kind == "int":
            return self.literal_sql("int", rng.choice([0, 1, 2, 3, 10, 100, -1, -5, 1000, 2**31, -(2**31), 2**40]))
        if kind == "dec":
            digits = rng.choice([1, 2, 3, 4])
            raw = rng.randint(-10**5, 10**5)
            return self.literal_sql("dec", decimal.Decimal(raw).scaleb(-digits))
        if kind == "dbl":
            return self.literal_sql("dbl", rng.choice([0.5, 0.25, -1.5, 2.75, 100.0, 0.125, -0.0625]))
        if kind == "date":
            days = rng.randint(0, 12000)
            return self.literal_sql("date", datetime.date(2000, 1, 1) + datetime.timedelta(days=days))
        if kind == "str":
            return self.literal_sql("str", rng.choice(["", "a", "b", "abc", "s1", "w3", "Z", "b'c", "zz"]))
        return "TRUE"

    def fitting_literal(self, kind, table, column, against):
        """A literal of the kind to compare with a value of type `against` ((precision, scale) or
        None): one whose common type with it fits 38 digits."""
        for _ in range(8):
            text = self.random_literal(kind, table, column)
            if kind not in ("int", "dec") or against is None:
                return text
            if common_fits(literal_ps(text), against):
                return text
        return "0"

    def numeric_literal(self):
        """A literal operand of arithmetic. It is small, so that it cannot overflow a column of
        any type; a plain query may also use the boundary literals."""
        kind = self.rng.choice(["int", "int", "dec", "dbl"])
        if kind == "int" and not self.hazard_ok:
            text = self.literal_sql("int", self.rng.randint(-1000, 1000))
        elif kind == "int" and self.chance(0.3):
            # The INTEGER boundary, where DuckDB and quarry once typed a literal differently.
            text = self.literal_sql("int", self.rng.choice([-(2**31), 2**31 - 1, 2**31, -(2**31) + 1]))
        else:
            text = self.random_literal(kind)
        ps = literal_ps(text) if kind in ("int", "dec") else None
        return Expr(text, kind, small=True, exact=True, decimal=kind == "dec", ps=ps)

    # ---- value expressions ------------------------------------------------------------------

    def numeric_leaf(self, edge_ok, bare=False):
        """A numeric column or literal. Edge columns take part in arithmetic only in a plain query;
        a bare comparison operand may be any column."""
        edge_ok = edge_ok and (self.hazard_ok or bare)
        roles = ["small", "key", "edge"] if edge_ok else ["small", "key"]
        pick = self.pick_column(kinds=("int", "dec", "dbl"), roles=roles)
        if pick is None or self.chance(0.12):
            return self.numeric_literal()
        alias, table, column = pick
        if column.role == "edge" and not bare:
            self.feat("arith:edge")
        return self.column_expr(alias, column)

    def numeric_expr(self, depth, edge_ok=False, exact_only=False):
        """A numeric expression: a column or literal, or arithmetic on up to two levels."""
        rng = self.rng
        if depth <= 0 or self.chance(0.35):
            leaf = self.numeric_leaf(edge_ok)
            if self.chance(0.08) and not exact_only:
                self.feat("arith:neg")
                return Expr(f"-{leaf.sql}" if leaf.column is not None or leaf.sql[0] != "-" else f"-({leaf.sql})",
                            leaf.kind, leaf.small, leaf.exact, decimal=leaf.decimal, ps=leaf.ps)
            return leaf
        op = rng.choice(["+", "-", "*"])
        if op == "*":
            left = self.numeric_expr(0, edge_ok=False)
            right = self.numeric_expr(0, edge_ok=False)
            if exact_only:
                # A product of doubles is rounded: multiply a double only by a small integer literal.
                if left.kind == "dbl" or right.kind == "dbl":
                    factor = str(rng.randint(2, 100))
                    base = left if left.kind == "dbl" else right
                    return Expr(f"({base.sql} * {factor})", "dbl", exact=True)
            self.feat("arith:mul")
            kind = _promote(left.kind, right.kind)
            return Expr(f"({left.sql} * {right.sql})", kind, small=False,
                        exact=not (left.kind == "dbl" and right.kind == "dbl"),
                        decimal=left.decimal or right.decimal, ps=combine_ps("*", kind, left, right))
        left = self.numeric_expr(depth - 1, edge_ok, exact_only)
        right = self.numeric_expr(depth - 1, edge_ok, exact_only)
        self.feat("arith:add" if op == "+" else "arith:sub")
        kind = _promote(left.kind, right.kind)
        # A DECIMAL turned into a DOUBLE is usually not a binary fraction, so sums of it round.
        mixes_decimal = kind == "dbl" and "dec" in (left.kind, right.kind)
        return Expr(f"({left.sql} {op} {right.sql})", kind,
                    small=left.small and right.small,
                    exact=left.exact and right.exact and not mixes_decimal,
                    decimal=left.decimal or right.decimal, ps=combine_ps(op, kind, left, right))

    def date_expr(self):
        """A DATE value: a column, or a column plus or minus a number of days."""
        pick = self.pick_column(kinds=("date",))
        alias, table, column = pick
        base = self.column_expr(alias, column)
        if column.role != "edge" and self.chance(0.2):
            self.feat("arith:date_plus_int")
            op = self.rng.choice(["+", "-"])
            return Expr(f"({base.sql} {op} {self.rng.randint(0, 400)})", "date")
        return base

    # ---- predicates -------------------------------------------------------------------------

    def predicate(self, depth=0):
        rng = self.rng
        r = rng.random()
        if depth < 2 and r < 0.25:
            op = rng.choice(["AND", "OR"])
            self.feat(f"pred:{op.lower()}")
            return f"({self.predicate(depth + 1)} {op} {self.predicate(depth + 1)})"
        if depth < 3 and r < 0.33:
            self.feat("pred:not")
            return f"NOT ({self.predicate(depth + 1)})"
        return self.atom()

    def comparison_op(self):
        op = self.rng.choice(list(COMPARISONS))
        self.feat(f"pred:{COMPARISONS[op]}")
        return op

    def atom(self):
        rng = self.rng
        kinds = rng.choices(
            ["num", "date", "str", "null", "between", "in", "bool"],
            weights=[30, 12, 10, 10, 14, 14, 4],
        )[0]
        if kinds == "num":
            return self.numeric_comparison()
        if kinds == "date":
            return self.date_comparison()
        if kinds == "str":
            return self.string_comparison()
        if kinds == "null":
            return self.null_test()
        if kinds == "between":
            return self.between()
        if kinds == "in":
            return self.in_list()
        return self.bool_atom()

    def note_wide(self, *exprs):
        for e in exprs:
            if e.column is not None and e.column.name in ("ew", "ev"):
                self.feat("pred:wide_decimal")

    def numeric_operand(self):
        """A numeric comparison operand: a column of any role (a bare column cannot overflow), or
        arithmetic."""
        if self.chance(0.6):
            alias, table, column = self.pick_column(kinds=("int", "dec", "dbl"))
            return self.column_expr(alias, column)
        return self.numeric_expr(1, edge_ok=False)

    def numeric_comparison(self):
        rng = self.rng
        for _ in range(8):
            left = self.numeric_operand()
            if rng.random() < 0.40:
                # Against another operand, preferring a DECIMAL column of another scale.
                if left.kind == "dec" and left.column is not None and self.chance(0.7):
                    options = [
                        (a, t, c) for a, t, c in self.columns(kinds=("dec",)) if c.scale != left.column.scale
                    ] or self.columns(kinds=("dec",))
                    alias, table, column = rng.choice(options)
                    right = self.column_expr(alias, column)
                else:
                    right = self.numeric_operand()
                if left.kind != "dbl" and right.kind != "dbl" and not common_fits(left.ps, right.ps):
                    continue
                if (left.column is not None and right.column is not None and left.kind == "dec"
                        and right.kind == "dec" and left.column.scale != right.column.scale):
                    self.feat("pred:cross_scale_decimal")
                self.note_wide(left, right)
                return f"{left.sql} {self.comparison_op()} {right.sql}"
            if left.column is not None:
                table = self.table_of[left.sql.split(".")[0]]
                literal = self.fitting_literal(left.kind, table, left.column, left.ps)
            else:
                kind = rng.choice(["int", "dec", "dbl"]) if left.kind != "dbl" else "dbl"
                literal = self.fitting_literal(kind, None, None, left.ps)
            self.note_wide(left)
            return f"{left.sql} {self.comparison_op()} {literal}"
        alias, table, column = self.pick_column(kinds=("int",), roles=("small", "key"))
        return f"{alias}.{column.name} {self.comparison_op()} 0"

    def interval_literal(self):
        rng = self.rng
        base = datetime.date(2000, 1, 1) + datetime.timedelta(days=rng.randint(0, 11000))
        n = rng.randint(1, 400)
        unit = rng.choice(["DAY", "MONTH", "YEAR"])
        if unit == "YEAR":
            n = rng.randint(1, 8)
        elif unit == "MONTH":
            n = rng.randint(1, 40)
        op = rng.choice(["+", "-"])
        self.feat("pred:date_interval")
        return f"DATE '{base.isoformat()}' {op} INTERVAL '{n}' {unit}"

    def date_comparison(self):
        rng = self.rng
        left = self.date_expr()
        self.feat("pred:date")
        r = rng.random()
        if r < 0.25:
            right = self.date_expr().sql
        elif r < 0.45:
            right = self.interval_literal()
        else:
            table = next((t for a, t in self.tables if left.sql.lstrip("(").startswith(a + ".")), None)
            column = left.column if left.column is not None else None
            right = self.random_literal("date", table, column)
        return f"{left.sql} {self.comparison_op()} {right}"

    def string_comparison(self):
        alias, table, column = self.pick_column(kinds=("str",))
        left = self.column_expr(alias, column)
        self.feat("pred:string")
        if self.chance(0.2):
            a2, t2, c2 = self.pick_column(kinds=("str",))
            right = f"{a2}.{c2.name}"
        else:
            right = self.random_literal("str", table, column)
        return f"{left.sql} {self.comparison_op()} {right}"

    def null_test(self):
        pick = self.pick_column()
        alias, table, column = pick
        expr = self.column_expr(alias, column).sql
        if self.chance(0.2) and column.kind in ("int", "dec", "dbl"):
            expr = self.numeric_expr(1, edge_ok=False).sql
        if self.chance(0.5):
            self.feat("pred:is_null")
            return f"{expr} IS NULL"
        self.feat("pred:is_not_null")
        return f"{expr} IS NOT NULL"

    def operand_for(self, kinds):
        alias, table, column = self.pick_column(kinds=kinds)
        return alias, table, column

    def between(self):
        rng = self.rng
        kind = rng.choice(["num", "num", "date", "str"])
        if kind == "num":
            alias, table, column = self.pick_column(kinds=("int", "dec", "dbl"))
            expr = self.column_expr(alias, column)
            lk = column.kind
            low = self.fitting_literal(lk, table, column, expr.ps)
            high = self.fitting_literal(lk, table, column, expr.ps)
            self.note_wide(expr)
        elif kind == "date":
            alias, table, column = self.pick_column(kinds=("date",))
            expr = self.column_expr(alias, column)
            low = self.random_literal("date", table, column)
            high = self.random_literal("date", table, column)
            self.feat("pred:date")
        else:
            alias, table, column = self.pick_column(kinds=("str",))
            expr = self.column_expr(alias, column)
            low = self.random_literal("str", table, column)
            high = self.random_literal("str", table, column)
            self.feat("pred:string")
        negated = self.chance(0.35)
        self.feat("pred:not_between" if negated else "pred:between")
        return f"{expr.sql} {'NOT ' if negated else ''}BETWEEN {low} AND {high}"

    def in_list(self):
        rng = self.rng
        kind = rng.choice(["int", "int", "dec", "str", "date", "dbl"])
        alias, table, column = self.pick_column(kinds=(kind,))
        expr = self.column_expr(alias, column)
        items = [self.fitting_literal(kind, table, column, expr.ps) for _ in range(rng.randint(1, 5))]
        if self.chance(0.1):
            items.insert(rng.randint(0, len(items)), "NULL")
            self.feat("pred:in_with_null")
        negated = self.chance(0.35)
        self.feat("pred:not_in" if negated else "pred:in")
        self.note_wide(expr)
        return f"{expr.sql} {'NOT ' if negated else ''}IN ({', '.join(items)})"

    def bool_atom(self):
        alias, table, column = self.pick_column(kinds=("bool",))
        ref = f"{alias}.{column.name}"
        self.feat("pred:bool_column")
        r = self.rng.random()
        if r < 0.4:
            return ref
        if r < 0.7:
            self.feat("pred:eq")
            return f"{ref} = {self.rng.choice(['TRUE', 'FALSE'])}"
        self.feat("pred:ne")
        return f"{ref} <> {self.rng.choice(['TRUE', 'FALSE'])}"

    def prunable_predicate(self):
        """A comparison on the column a table is sorted by, so that row groups can be skipped."""
        sorted_tables = [(a, t) for a, t in self.tables if t.sorted_by]
        if not sorted_tables:
            return None
        alias, table = self.rng.choice(sorted_tables)
        column = table.column(table.sorted_by)
        op = self.rng.choice(["=", "<", "<=", ">", ">="])
        literal = self.random_literal(column.kind, table, column)
        self.feat("pred:prunable")
        self.feat(f"pred:{COMPARISONS[op]}")
        return f"{alias}.{column.name} {op} {literal}"

    # ---- aggregates -------------------------------------------------------------------------

    def aggregate(self, allow_avg_decimal=True, edge=True):
        """Returns an Aggregate. SUM and AVG take numbers; MIN and MAX anything ordered; COUNT
        anything. With edge=False the argument has no arithmetic on edge columns."""
        rng = self.rng
        name = rng.choices(AGGREGATES, weights=[22, 14, 14, 10, 20, 20])[0]
        if name == "count_star":
            self.feat("agg:count_star")
            return Aggregate("COUNT(*)", "count")
        if name in ("sum", "avg"):
            arg = self.numeric_expr(rng.choice([0, 0, 1]), edge_ok=edge and self.chance(0.3), exact_only=True)
            for _ in range(8):
                if arg.exact:
                    break
                arg = self.numeric_expr(rng.choice([0, 0, 1]), edge_ok=edge and self.chance(0.3), exact_only=True)
            else:
                alias, table, column = self.pick_column(kinds=("int", "dec"), roles=("small", "key"))
                arg = self.column_expr(alias, column)
            if name == "avg" and arg.decimal and not allow_avg_decimal:
                name = "sum"
            self.feat(f"agg:{name}")
            return Aggregate(f"{name.upper()}({arg.sql})", name, approx=name == "avg")
        if name == "count":
            alias, table, column = self.pick_column()
            self.feat("agg:count")
            return Aggregate(f"COUNT({alias}.{column.name})", "count")
        alias, table, column = self.pick_column(kinds=("int", "dec", "dbl", "date", "str"))
        self.feat(f"agg:{name}")
        return Aggregate(f"{name.upper()}({alias}.{column.name})", name, table=table, column=column)

    def having(self, group_exprs):
        rng = self.rng
        parts = []
        for _ in range(rng.randint(1, 2)):
            if group_exprs and self.chance(0.3):
                expr, kind, table, column = rng.choice(group_exprs)
                if kind in ("int", "dec", "str", "date"):
                    literal = self.fitting_literal(kind, table, column, _column_ps(column))
                    parts.append(f"{expr} {self.comparison_op()} {literal}")
                    continue
            agg = self.aggregate(allow_avg_decimal=False, edge=False)
            if agg.name in ("count", "count_star"):
                parts.append(f"{agg.sql} {self.comparison_op()} {rng.randint(0, 6)}")
            elif agg.name in ("min", "max"):
                if self.chance(0.3):
                    parts.append(f"{agg.sql} IS NOT NULL")
                    self.feat("pred:is_not_null")
                else:
                    literal = self.fitting_literal(agg.column.kind, agg.table, agg.column, _column_ps(agg.column))
                    parts.append(f"{agg.sql} {self.comparison_op()} {literal}")
            elif agg.name == "sum":
                parts.append(f"{agg.sql} {self.comparison_op()} {self.literal_sql('int', rng.randint(-1000, 1000))}")
            else:  # AVG of an integer or a DOUBLE; a DECIMAL's AVG was turned into a SUM above
                parts.append(f"{agg.sql} {self.comparison_op()} {self.random_literal(rng.choice(['int', 'dbl']))}")
        if len(parts) == 2:
            op = rng.choice(["AND", "OR"])
            self.feat(f"pred:{op.lower()}")
            return f"{parts[0]} {op} {parts[1]}"
        return parts[0]


@dataclass
class Aggregate:
    sql: str
    name: str  # sum, avg, count, count_star, min, max
    approx: bool = False  # AVG: compared with the tolerance
    table: object = None  # of the argument column, for MIN and MAX
    column: object = None


def _promote(a, b):
    order = {"int": 0, "dec": 1, "dbl": 2}
    return a if order[a] >= order[b] else b


def literal_ps(text):
    """(precision, scale) DuckDB and quarry give a numeric literal: 12.50 is (4, 2), 7 is (10, 0)."""
    body = text.lstrip("-")
    if "." in body:
        whole, fraction = body.split(".")
        return max(len(whole.lstrip("0")), 1) + len(fraction), len(fraction)
    return (10, 0) if int(body) < 2**31 else (19, 0)


def common_fits(a, b):
    """Whether comparing values of the (precision, scale) types a and b stays within 38 digits."""
    return max(a[0] - a[1], b[0] - b[1]) + max(a[1], b[1]) <= 38


def combine_ps(op, kind, left, right):
    if kind == "dbl":
        return None
    if kind == "int":
        return (19, 0)
    (p1, s1), (p2, s2) = left.ps, right.ps
    if op == "*":
        scale = s1 + s2
        width = p1 + p2
        if p1 <= 18 and p2 <= 18 and width > 18:
            width = 18
        return min(max(width, scale), 38), scale
    scale = max(s1, s2)
    return min(max(p1 - s1, p2 - s2) + scale + 1, 38), scale


FAMILIES = {"int": ["k1", "k2"], "str": ["ks"], "date": ["kd"], "dec": ["kc"]}


def _join_condition(rng, left_alias, left_table, right_alias, right_table, builder, family=None):
    family = family or rng.choice(["int", "int", "str", "date", "dec"])
    lc = rng.choice(FAMILIES[family])
    rc = rng.choice(FAMILIES[family])
    return f"{left_alias}.{lc} = {right_alias}.{rc}"


def _pick_tables(rng, dataset, count):
    chosen = []
    pool = list(dataset.tables)
    special = [t for t in pool if t.rows <= 1]
    first = rng.choice(special) if special and rng.random() < 0.5 else rng.choice(pool)
    chosen.append(first)
    for _ in range(count - 1):
        chosen.append(rng.choice(pool))
    # Shuffle only so that the special table is not always first.
    if rng.random() < 0.5:
        rng.shuffle(chosen)
    return chosen


def generate_query(seed, index, dataset, rng):
    builder_rng = rng
    count = rng.choices([1, 2, 3, 4], weights=[26, 30, 24, 20])[0]
    chosen = _pick_tables(rng, dataset, count)
    tables = [(f"q{i}", t) for i, t in enumerate(chosen)]
    plain = count == 1 and rng.random() < 0.35  # no WHERE: both engines evaluate every row
    b = Builder(rng, tables, hazard_ok=plain)
    b.feat(f"join:{count}")
    if len({id(t) for t in chosen}) < len(chosen):
        b.feat("join:self")

    # FROM clause and join conditions.
    conditions = []  # (new alias index, condition text)
    for i in range(1, count):
        j = rng.randrange(i)
        la, lt = tables[j]
        ra, rt = tables[i]
        pairs = [_join_condition(rng, la, lt, ra, rt, b)]
        if rng.random() < 0.22:
            pairs.append(_join_condition(rng, la, lt, ra, rt, b))
            b.feat("join:multi_key")
        if i >= 2 and rng.random() < 0.3:
            k = rng.choice([x for x in range(i) if x != j])
            ka, kt = tables[k]
            pairs.append(_join_condition(rng, ka, kt, ra, rt, b))
            b.feat("join:cycle")
        conditions.append((i, pairs))
    comma = count > 1 and rng.random() < 0.4
    filters = []
    if not plain and rng.random() < 0.7:
        for _ in range(rng.choice([1, 1, 2, 3])):
            filters.append(b.predicate())
    if not plain and rng.random() < 0.3:
        prunable = b.prunable_predicate()
        if prunable:
            filters.append(prunable)

    from_sql = f"{tables[0][1].name} AS {tables[0][0]}"
    where_parts = []
    if comma:
        b.feat("join:comma")
        for alias, table in tables[1:]:
            from_sql += f", {table.name} AS {alias}"
        for _, pairs in conditions:
            where_parts.extend(pairs)
    else:
        if count > 1:
            b.feat("join:on")
        for (i, pairs) in conditions:
            alias, table = tables[i]
            from_sql += f" {rng.choice(['JOIN', 'INNER JOIN'])} {table.name} AS {alias} ON {' AND '.join(pairs)}"
    where_parts.extend(filters)
    where_sql = " AND ".join(where_parts) if len(where_parts) <= 1 else " AND ".join(f"({p})" for p in where_parts)
    if filters:
        b.feat("filter")

    aggregate_query = rng.random() < 0.5
    approx = set()
    items = []
    group_by_sql = ""
    having_sql = ""
    order_keys = []

    if aggregate_query:
        group_exprs = []
        n_keys = rng.choices([0, 1, 2, 3], weights=[18, 38, 28, 16])[0]
        if n_keys == 0:
            b.feat("global_aggregate")
        else:
            b.feat(f"group_by:{n_keys}")
        seen = set()
        for _ in range(n_keys):
            for _attempt in range(5):
                if rng.random() < 0.2:
                    alias_, table, column = b.pick_column(kinds=("int",), roles=("small", "key", "edge"))
                    sql = f"{alias_}.{column.name} + {rng.randint(1, 9)}" if column.role != "edge" else f"{alias_}.{column.name}"
                    kind = column.kind
                    if column.role != "edge":
                        b.feat("group_by:expression")
                        b.feat("arith:add")
                else:
                    alias_, table, column = b.pick_column(roles=("key", "key", "small", "edge", "value"))
                    sql = f"{alias_}.{column.name}"
                    kind = column.kind
                if sql not in seen:
                    seen.add(sql)
                    group_exprs.append((sql, kind, table, column))
                    break
        for i, (sql, kind, table, column) in enumerate(group_exprs):
            items.append((sql, f"g{i}"))
            order_keys.append(f"g{i}")
        for i in range(rng.randint(1, 3)):
            agg = b.aggregate()
            index_ = len(items)
            items.append((agg.sql, f"a{i}"))
            order_keys.append(f"a{i}")
            if agg.approx:
                approx.add(index_)
        if n_keys > 0:
            group_by_sql = " GROUP BY " + ", ".join(g[0] for g in group_exprs)
            if rng.random() < 0.35:
                b.feat("having")
                having_sql = " HAVING " + b.having(group_exprs)
    else:
        b.feat("projection")
        for i in range(rng.randint(1, 5)):
            r = rng.random()
            if r < 0.45:
                alias_, table, column = b.pick_column()
                items.append((f"{alias_}.{column.name}", f"o{i}"))
            elif r < 0.9:
                items.append((b.numeric_expr(rng.choice([1, 2]), edge_ok=rng.random() < 0.15).sql, f"o{i}"))
            else:
                items.append((b.predicate(1), f"o{i}"))
        order_keys = [alias for _, alias in items]
        rng.shuffle(order_keys)

    order_sql = ""
    limit_sql = ""
    ordered = False
    if rng.random() < 0.45 and order_keys:
        ordered = True
        b.feat("order_by")
        parts = []
        for key in order_keys:
            part = key
            if rng.random() < 0.6:
                part += rng.choice([" ASC", " DESC"])
            if rng.random() < 0.2:
                part += rng.choice([" NULLS FIRST", " NULLS LAST"])
            parts.append(part)
        order_sql = " ORDER BY " + ", ".join(parts)
        if rng.random() < 0.5:
            b.feat("limit")
            limit_sql = f" LIMIT {rng.choice([0, 1, 2, 5, 10, 50, 200])}"

    select_sql = ", ".join(f"{sql} AS {alias}" for sql, alias in items)
    sql = f"SELECT {select_sql} FROM {from_sql}"
    if where_sql:
        sql += f" WHERE {where_sql}"
    sql += group_by_sql + having_sql + order_sql + limit_sql
    count_sql = f"SELECT COUNT(*) FROM {from_sql}" + (f" WHERE {where_sql}" if where_sql else "")

    options = Options(
        batch_size=rng.choices(BATCH_SIZES, weights=[25, 25, 50])[0],
        memory_limit=rng.choices(MEMORY_LIMITS, weights=[60, 40])[0],
        disabled_rule=rng.choice([None, None] + RULES),
    )
    features = list(b.features.elements()) + options.features()
    from_count_sql = f"SELECT COUNT(*) FROM {from_sql}"
    return Query(seed, index, sql, count_sql, ordered, frozenset(approx), options, features, from_count_sql)


def generate_queries(seed, dataset):
    rng = random.Random(f"quarry-query-{seed}")
    return [generate_query(seed, i, dataset, rng) for i in range(QUERIES_PER_SEED)]
