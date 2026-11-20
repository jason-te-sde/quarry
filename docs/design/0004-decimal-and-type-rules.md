# ADR 0004: Decimal representation and the type rules

Status: accepted.

## Decision

### Representation

`DECIMAL(p, s)` is the integer `value * 10^s`.

- p <= 18: an `int64_t`, in an Arrow `decimal64(p, s)` array.
- 18 < p <= 38: a 128-bit integer (`__int128`, named `Int128`), in an Arrow `decimal128(p, s)`
  array. Arrow's little-endian layout of a `decimal128` is the layout of the compiler's `__int128`.
- The Parquet reader returns `decimal128` for every decimal column, p <= 18 included.
  `LogicalType::FromArrow` accepts it, and the evaluator narrows such a column to 64 bits when it
  reads it (a value that does not fit is an error).

Arithmetic never wraps. Every `+ - *` and every cast checks two things: that the machine integer did
not overflow (`__builtin_*_overflow`), and that the result has at most `p` digits of the type it is
stored in. A failed check is `arrow::Status::Invalid("decimal overflow ...")`. Rows that are NULL in
an operand are not computed, so a NULL never raises one.

Rescaling (cast between scales, comparison against another scale) multiplies by a power of ten when
the scale grows and divides when it shrinks. Dividing rounds **half away from zero**, which is what
DuckDB 2.0.1 does (`CAST(1.005 AS DECIMAL(4,2))` is `1.01`, `CAST(-2.5 AS DECIMAL(2,0))` is `-3`;
see the probe table in `docs/sql.md`). The remainder is compared as `|r| >= divisor - |r|`, which
cannot overflow even when the divisor is 10^38.

`DECIMAL` to `DOUBLE` is `double(raw) / 10^scale` with the power of ten taken from a table of the
doubles nearest to 10^0 .. 10^38.

### The type table

The rules are the ones DuckDB 2.0.1 prints with `typeof()`. They are not written here: they are
generated into `docs/sql.md` by `scripts/duckdb_types.py` (which records the DuckDB version), and
`TypeRulesTest.DocsTableMatchesTheRules` asserts every row of that table against
`src/types/type_rules.cc`. Upgrading DuckDB regenerates the table, and a changed rule fails the test
instead of going unnoticed.

### Where the first design differs from DuckDB, and DuckDB wins

The design that preceded the measurement guessed some rules. Probing DuckDB 2.0.1 settled them:

| Rule | First guess | DuckDB 2.0.1 (what quarry does) |
| --- | --- | --- |
| `DECIMAL(15,2) * DECIMAL(15,2)` | `DECIMAL(30,4)` (`p1 + p2`) | `DECIMAL(18,4)`: when both operands fit 64 bits and `p1 + p2` exceeds 18, the width is 18 |
| `DECIMAL(31,4) * DECIMAL(16,2)` | `DECIMAL(38,6)` | `DECIMAL(38,6)`, as guessed (an operand beyond 64 bits keeps the full width, capped at 38) |
| width of a product | -- | never below its scale: `DECIMAL(18,18) * DECIMAL(18,18)` is `DECIMAL(36,36)` |
| `SUM(INTEGER)` | `DECIMAL(38,0)`; DuckDB says `HUGEINT` | `HUGEINT` in DuckDB, `DECIMAL(38,0)` in quarry (the one intentional difference: no `HUGEINT` type; the comparator compares by value) |
| `DATE + INTERVAL 1 MONTH` | `DATE` | `TIMESTAMP` in DuckDB; quarry folds the interval at plan time and stays `DATE` |
| `NULL` order | `NULLS LAST` | `NULLS LAST` ascending *and* descending |
| overflow of a 64-bit product | -- | an error at 10^18, `Overflow in multiplication of DECIMAL(18)` |

The multiplication rule has a consequence worth stating. A product of two `DECIMAL(15,2)` columns
is a 64-bit decimal that overflows once the product reaches 10^18 (unscaled factors of about 10^9
each), where the guessed rule would have gone to 128 bits. quarry follows DuckDB, so a query that overflows in DuckDB overflows
in quarry, with the same kind of error, and the differential test never has to decide which of the
two answers is right.

### Comparison types

Comparisons cast both operands to one common type before a kernel runs (the kernels take two
arrays of the same type). `CommonComparisonType`:

- the same type: itself; `INTEGER` with `BIGINT`: `BIGINT`;
- any number with `DOUBLE`: `DOUBLE`;
- `DECIMAL` with `DECIMAL` or an integer: the larger scale and the larger count of integer digits,
  capped at 38 (so `1.50 = 1.5` and `DECIMAL(15,2) < INTEGER` are exact);
- anything else (`VARCHAR` with a number, `DATE` with an integer, ...) is not comparable.

The factory functions in `src/expr/bound_expr.h` insert the casts, so a tree they produce always
satisfies what the kernels require, and a hand-built tree that does not is an `Invalid` status
instead of a misread buffer.

### Aggregates

`SUM(DECIMAL(p,s))` is `DECIMAL(38,s)` accumulated in 128 bits; `SUM` of an integer is
`DECIMAL(38,0)`; `SUM(DOUBLE)` is `DOUBLE`; `AVG` of any number is `DOUBLE`; `COUNT` is `BIGINT`;
`MIN` and `MAX` keep the type.

## Consequences

- Overflow is a visible error, never a wrong number, and the property test checks that the
  vectorized and the reference evaluators agree on *whether* an expression overflows.
- Anything quarry reports as an error and DuckDB does not (or the reverse) is a bug in the type
  table or a kernel, and the generated table is where to look first.
