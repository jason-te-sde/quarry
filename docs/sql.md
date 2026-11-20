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
| `DATE` | `date32` | days since 1970-01-01, years 0001 to 9999 |
| `VARCHAR` | `utf8` | compared byte by byte |

A Parquet `DECIMAL` column is read as `decimal128` whatever its precision; quarry accepts it as
`DECIMAL(p, s)` and narrows it to 64 bits when p <= 18. Columns of any other Parquet type (FLOAT,
UINT64, TIMESTAMP, LIST, DECIMAL with p > 38) are not supported.

There is no `/` operator in v0.1: `AVG` is an aggregate, not a division.

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
