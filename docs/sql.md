# SQL dialect

What quarry accepts and how it types it. DuckDB, pinned in `uv.lock`, is the reference: wherever
this page says "DuckDB", it means the version recorded in the table below, and the table is
generated, not typed. The reasons behind each rule are in
[ADR 0004](design/0004-decimal-and-type-rules.md).

## Types

| SQL type | Arrow type | Notes |
| --- | --- | --- |
| `BOOLEAN` | `bool` | three-valued: TRUE, FALSE, NULL |
| `INTEGER` | `int32` | arithmetic that leaves the range is an error, not a wraparound |
| `BIGINT` | `int64` | the same |
| `DECIMAL(p, s)` | `decimal64(p, s)` for p <= 18, `decimal128(p, s)` for p <= 38 | stored as the integer value * 10^s; overflow is an error |
| `DOUBLE` | `float64` | IEEE 754; no NaN or infinity is produced by quarry's own literals |
| `DATE` | `date32` | days since 1970-01-01, years 0001 to 9999; adding or subtracting a number of days is not range-checked, so a result outside those years is not detected |
| `VARCHAR` | `utf8` | compared byte by byte |

A Parquet `DECIMAL` column is read as `decimal128` whatever its precision; quarry accepts it as
`DECIMAL(p, s)` and narrows it to 64 bits when p <= 18. Columns of any other Parquet type (FLOAT,
UINT64, TIMESTAMP, LIST, DECIMAL with p > 38) are not supported. A file written by Polars, or by pandas
with pyarrow's string types, can record `large_string` or `string_view` in the Arrow schema it stores in
its metadata; quarry reads that schema, reports such a column as unsupported and leaves the file's other
columns usable.

There is no `/` operator in v0.1: `AVG` is an aggregate, not a division.

A `DATE` is not compared with a string: `WHERE l_shipdate >= '1994-01-01'` is the binder error
`cannot compare DATE with VARCHAR`. Write `DATE '1994-01-01'`.

## Type rules

Numbers meet as follows. An `INTEGER` meeting a `DECIMAL` is `DECIMAL(10,0)`, a `BIGINT` is
`DECIMAL(19,0)`.

- `INTEGER op BIGINT` is `BIGINT`; `DOUBLE` with any number is `DOUBLE`.
- `DECIMAL + DECIMAL` and `DECIMAL - DECIMAL`: scale `max(s1, s2)`, width
  `max(p1 - s1, p2 - s2) + max(s1, s2) + 1`, capped at 38.
- `DECIMAL * DECIMAL`: scale `s1 + s2`, width `p1 + p2`; when both operands fit 64 bits and the sum
  exceeds 18 the width is 18 (DuckDB keeps the product in 64 bits and checks for overflow); the
  width is capped at 38 and never below the scale.
- Comparisons convert both sides to a common type: for decimals the larger scale and the larger
  integer-digit count (`1.50 = 1.5` is TRUE).
- `SUM(DECIMAL(p, s))` is `DECIMAL(38, s)`; `SUM` of an integer is `DECIMAL(38, 0)`; `AVG` is
  `DOUBLE`; `COUNT` is `BIGINT`; `MIN` and `MAX` keep the argument type.
- `DATE +/- INTEGER` is `DATE` (days). `DATE +/- INTERVAL` is folded by the planner into a `DATE`
  with month and year steps clamped to the last day of the month.
- Rescaling a decimal rounds half away from zero (`CAST(1.005 AS DECIMAL(4,2))` is `1.01`,
  `CAST(-2.5 AS DECIMAL(2,0))` is `-3`). A value that does not fit the target precision is an
  error.
- `NULL` sorts last, ascending and descending.

## Grammar

One `SELECT` statement, optionally ended by `;`. Keywords are case-insensitive, `--` starts a comment
that runs to the end of the line.

```ebnf
statement   = "SELECT" item { "," item } "FROM" from { "," from }
              [ "WHERE" expr ] [ "GROUP" "BY" expr { "," expr } ] [ "HAVING" expr ]
              [ "ORDER" "BY" key { "," key } ] [ "LIMIT" integer ] [ ";" ] ;
item        = expr [ [ "AS" ] alias ] ;
from        = table { [ "INNER" ] "JOIN" table "ON" expr } ;
table       = name [ [ "AS" ] alias ] ;
key         = expr [ "ASC" | "DESC" ] [ "NULLS" ( "FIRST" | "LAST" ) ] ;
expr        = or ;
or          = and { "OR" and } ;
and         = not { "AND" not } ;
not         = "NOT" not | predicate ;
predicate   = sum [ ( "=" | "<>" | "!=" | "<" | "<=" | ">" | ">=" ) sum
                  | "IS" [ "NOT" ] "NULL"
                  | [ "NOT" ] "BETWEEN" sum "AND" sum
                  | [ "NOT" ] "IN" "(" expr { "," expr } ")" ] ;
sum         = product { ( "+" | "-" ) product } ;
product     = unary { "*" unary } ;
unary       = ( "-" | "+" ) unary | primary ;
primary     = integer | decimal | string | "TRUE" | "FALSE" | "NULL"
            | "DATE" string | "INTERVAL" string ( "DAY" | "MONTH" | "YEAR" )
            | [ name "." ] name
            | ( "SUM" | "AVG" | "COUNT" | "MIN" | "MAX" ) "(" ( "*" | expr ) ")"
            | "(" expr ")" ;
```

Expressions may nest 256 levels deep (parentheses, chains of `NOT` or unary minus, and the height of
the expression tree, so a chain of 257 `+` is refused); a deeper statement is a syntax error that
says "nesting too deep". What is refused, and how, is listed under [Refused](#refused).

## Literals and identifiers

- An integer literal is `INTEGER` when it fits 32 bits and `BIGINT` when it fits 64; a larger one is
  a binder error ("integer literal ... is out of range"). `-9223372036854775808` is accepted.
- A decimal literal has a point followed by digits (`.5` and `0.05` are both fine, `1.` is not); its
  precision is its number of digits and its scale the number after the point, so `0.05` is
  `DECIMAL(3,2)`. There is no exponent
  notation: `1e5` is a syntax error ("a number may not be followed by 'e'").
- A string literal is single-quoted, a quote inside it is doubled (`'it''s'`), and its type is
  `VARCHAR`.
- `TRUE` and `FALSE` are `BOOLEAN`. A bare `NULL` is an `INTEGER` NULL, as in DuckDB, and takes the type
  of the other operand when it sits next to one (`x = NULL`, `x IN (1, NULL)`, `BETWEEN NULL AND 5`).
- `DATE 'YYYY-MM-DD'` is a `DATE`; a date that does not exist is an error. `INTERVAL 'n' DAY`, `MONTH`
  or `YEAR` takes an integer of at most nine digits and may only follow `+` or `-` after a constant
  `DATE` (see Names).
- A double-quoted identifier may contain spaces and other characters, a doubled quote inside it stands for one, and it
  may not be empty. A reserved word that quarry does not implement (below) is usable as a name only
  when quoted.

## Predicates and aggregates

- `WHERE`, `HAVING` and `JOIN ... ON` conditions must be `BOOLEAN`. A row for which the condition is
  `NULL` is dropped.
- The values of an `IN` list must be constants (literals, or a `DATE` literal with an interval, which
  is folded); an expression, a column or a subquery there is a binder error. They take the type of
  the left side. `NOT IN` and `NOT BETWEEN` are the negation of `IN` and `BETWEEN`.
- In `JOIN ... ON`, each equality between an expression of the left input and one of the right input
  is a join key (the two sides are brought to a common type); every other condition of the `ON`
  clause is applied to the joined rows.
- The aggregates are `SUM`, `AVG`, `COUNT`, `MIN` and `MAX` over one expression, and `COUNT(*)`; any
  other function name is "unknown function", and `SUM(*)` is an error ("only COUNT(*) takes a star").
  Aggregates may appear in the select list, `HAVING` and `ORDER BY`. They may not appear in `WHERE`,
  `GROUP BY` or `ON` ("aggregate functions are not allowed in ..."), and they may not nest.
- A query with a `HAVING` clause or an aggregate anywhere but no `GROUP BY` is one group over all
  the rows that pass the `WHERE`. `COUNT` ignores `NULL`s; `COUNT(*)` counts rows. `SUM`, `AVG`, `MIN`
  and `MAX` of no non-`NULL` value are `NULL`.

## Refused

These words and constructs are not implemented. Each is refused when the statement is parsed or
bound, with a message that names it, and never by returning a different answer. The reasons are in
[ADR 0007](design/0007-scope.md).

| Refused | Message or behaviour |
| --- | --- |
| `LEFT`, `RIGHT`, `FULL`, `OUTER`, `CROSS`, `NATURAL`, `USING` | `<word> is not supported` (only `[INNER] JOIN ... ON` and the comma join exist) |
| `UNION`, `INTERSECT`, `EXCEPT`, `ALL`, `ANY` | `<word> is not supported` |
| `DISTINCT`, `LIKE`, `ILIKE`, `CASE`, `WHEN`, `THEN`, `ELSE`, `END` | `<word> is not supported` |
| `EXISTS`, `WITH`, `OVER`, `OFFSET` | `<word> is not supported` |
| `/` | `division is not supported` (`AVG` is an aggregate) |
| `SELECT *` | `SELECT * is not supported: list the columns` |
| a subquery, or a parenthesised `FROM` item | `subqueries are not supported`, `subqueries and parenthesised FROM items are not supported` |
| `GROUP BY 1`, `ORDER BY 1` | `GROUP BY ordinals are not supported: name the expression`, and the same for `ORDER BY` |
| an `INTERVAL` anywhere but after a constant `DATE` | `INTERVAL arithmetic needs a constant DATE on the left` |
| a cross product (comma join with no equality connecting a table) | `planner error ... requires a cross product between <a> and <b>` |
| exponent notation (`1e5`) | `invalid numeric literal` |
| a Parquet column of type `FLOAT`, `UINT64`, `TIMESTAMP`, `LIST` or `DECIMAL` with p > 38 | the table registers; naming the column is a binder error that gives its type and file |

## Names

- An unquoted identifier is case-insensitive (`SELECT L_QUANTITY FROM LineItem` works); a quoted one
  is exact, and table and column names are stored in lower case, so `"L_QUANTITY"` is an unknown
  column.
- An unqualified column that two FROM items have is a binder error that lists both candidates
  (`lineitem.x, orders.x`). A table alias hides the table's own name.
- A select alias can be used in `ORDER BY` but not in `WHERE`, `GROUP BY` or `HAVING`. An ordinal
  (`ORDER BY 1`, `GROUP BY 1`) is a binder error.
- `ORDER BY` takes an alias on its own, a select expression or any other expression over the table's
  columns; an alias inside an expression (`ORDER BY alias + 1`) is a binder error (`unknown column`),
  so write the select expression itself. `NULLS FIRST` and `NULLS LAST` are recorded on every sort
  key, and the default is `NULLS LAST` for both directions.
- In a query with aggregates or `GROUP BY`, a column outside an aggregate must be a `GROUP BY`
  expression (compared as a bound expression, so `GROUP BY a + 1` allows `SELECT a + 1`).
- `DATE '...' +/- INTERVAL 'n' DAY|MONTH|YEAR` is folded by the binder when the date is a literal;
  an interval anywhere else is a binder error.
- A table of several Parquet files registers even when the files disagree on a column's type, or a
  column has an unsupported type (FLOAT, UINT64, TIMESTAMP, LIST, DECIMAL with p > 38) or is
  missing from a later file: that column is unusable, and naming it is a binder error that gives
  the column, the types and the file. The other columns stay usable.
- An output column is named as the user typed it, like DuckDB does: `SELECT L_QUANTITY` yields a
  column `L_QUANTITY`, `SELECT x AS MyName` one called `MyName`, and any other select item is named
  by its text as written.
- Errors read `<kind> error at <line>:<column>: <message>`, kind `syntax`, `binder` or `planner`, and
  the position is the offending token; for the planner's "requires a cross product" it is where the
  relation that cannot be reached is written in the FROM clause.

## DuckDB's type table

Produced by running `uv run scripts/duckdb_types.py`; `TypeRulesTest.DocsTableMatchesTheRules` checks every
row against the rules in `src/types/type_rules.cc`.

<!-- duckdb-types -->
DuckDB version: 2.0.1

| Expression | typeof |
| --- | --- |
| `0.05` | `DECIMAL(3,2)` |
| `1` | `INTEGER` |
| `1 - 0.05` | `DECIMAL(13,2)` |
| `DECIMAL(15,2) * DECIMAL(15,2)` | `DECIMAL(18,4)` |
| `DECIMAL(31,4) * DECIMAL(16,2)` | `DECIMAL(38,6)` |
| `DECIMAL(15,2) + INTEGER` | `DECIMAL(16,2)` |
| `DECIMAL(15,2) + DECIMAL(10,4)` | `DECIMAL(18,4)` |
| `DECIMAL(15,2) - DECIMAL(10,4)` | `DECIMAL(18,4)` |
| `DECIMAL(38,0) * DECIMAL(38,0)` | `DECIMAL(38,0)` |
| `DECIMAL(12,5) * DECIMAL(12,5)` | `DECIMAL(18,10)` |
| `DECIMAL(18,2) * DECIMAL(19,2)` | `DECIMAL(37,4)` |
| `DECIMAL(18,18) * DECIMAL(18,18)` | `DECIMAL(36,36)` |
| `DECIMAL(38,10) * DECIMAL(38,10)` | `DECIMAL(38,20)` |
| `DECIMAL(38,2) + DECIMAL(38,2)` | `DECIMAL(38,2)` |
| `DECIMAL(15,2) * INTEGER` | `DECIMAL(18,2)` |
| `DECIMAL(15,2) + BIGINT` | `DECIMAL(22,2)` |
| `SUM(DECIMAL(15,2))` | `DECIMAL(38,2)` |
| `SUM(INTEGER)` | `HUGEINT` |
| `SUM(BIGINT)` | `HUGEINT` |
| `AVG(DECIMAL(15,2))` | `DOUBLE` |
| `AVG(INTEGER)` | `DOUBLE` |
| `MIN(DECIMAL(15,2))` | `DECIMAL(15,2)` |
| `MAX(DECIMAL(15,2))` | `DECIMAL(15,2)` |
| `COUNT(*)` | `BIGINT` |
| `DATE + INTERVAL 1 MONTH` | `TIMESTAMP` |
| `INTEGER + BIGINT` | `BIGINT` |
| `INTEGER * INTEGER` | `INTEGER` |
| `INTEGER + DOUBLE` | `DOUBLE` |
| `DECIMAL(15,2) + DOUBLE` | `DOUBLE` |
| `DECIMAL(15,2) = INTEGER` | `BOOLEAN` |

| Probe | Result |
| --- | --- |
| `ORDER BY x over (1, NULL)` | `NULLS LAST` |
| `ORDER BY x DESC over (1, NULL)` | `NULLS LAST` |
| `CAST(1.005 AS DECIMAL(4,2))` | `1.01` |
| `CAST(-1.005 AS DECIMAL(4,2))` | `-1.01` |
| `CAST(1.015 AS DECIMAL(4,2))` | `1.02` |
| `CAST(2.5 AS DECIMAL(2,0))` | `3` |
| `CAST(-2.5 AS DECIMAL(2,0))` | `-3` |
| `DATE '1994-01-31' + INTERVAL 1 MONTH` | `1994-02-28 00:00:00` |
| `DATE '1998-12-01' - INTERVAL 90 DAY` | `1998-09-02 00:00:00` |
<!-- /duckdb-types -->


### Where quarry differs from the table above, and from the design's first expectations

- `SUM(INTEGER)` and `SUM(BIGINT)`: DuckDB returns `HUGEINT`. quarry has no such type and returns
  `DECIMAL(38, 0)`, accumulated in 128 bits. The differential comparator compares results by value,
  not by type name. This is the one intentional difference.
- `DATE + INTERVAL 1 MONTH`: DuckDB returns `TIMESTAMP`. quarry folds the interval at plan time and
  keeps a `DATE`; the TPC-H queries only compare such a result with another date.
- `DECIMAL(15,2) * DECIMAL(15,2)` is `DECIMAL(18,4)` in DuckDB 2.0.1, not `DECIMAL(30,4)` as the
  initial design expected from `p1 + p2`: the 64-bit narrowing rule above. The consequence is that
  an overflow can happen at 10^18, not 10^30; quarry reports it as `decimal overflow` where DuckDB
  reports `Overflow in multiplication of DECIMAL(18)`. TPC-H Q1's `sum_charge` multiplies three
  `DECIMAL(15,2)` terms and lands on `DECIMAL(18,6)` per row, then `SUM` widens it to
  `DECIMAL(38,6)`.
- `NULL` order: DuckDB 2.0.1 puts NULLs last for `ORDER BY x` and also for `ORDER BY x DESC`. The
  design expected NULLS LAST by default; the descending case is what the probe table adds.
- Integer arithmetic that overflows (`2147483647 + 1` as `INTEGER`) is an error in DuckDB and in
  quarry, not a wraparound.
