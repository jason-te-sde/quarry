# Testing

What tests quarry has, what each layer is there to catch, and what no layer covers. DuckDB, pinned
in `uv.lock`, is the reference engine for everything that compares answers.

## The layers

| Layer | Where | What it catches |
| --- | --- | --- |
| Unit tests | `tests/unit/`, GoogleTest | a kernel, a rule or an operator wrong in isolation: decimal arithmetic, the lexer and parser, the binder's types, the optimizer rules, hash tables, spilling, the Parquet scan |
| Expression property test | `tests/unit/expr/eval_property_test.cc` | the vectorized evaluator disagreeing with a row-at-a-time reference evaluator (`tests/support/reference_eval.cc`) on random expression trees, selections included |
| SQL tests | `tests/sql/*.test` | syntax and binder errors with their positions; 58 query results generated from DuckDB (`scripts/gen_goldens.py`) |
| Plan goldens | `tests/planner/plans/` | an optimizer change that alters a plan nobody meant to alter |
| TPC-H | `tests/tpch/` | Q1, Q3, Q5 and Q6 at scale factor 0.01 against DuckDB's answers, and the same queries under 64 KiB with spilling, whose measured memory must stay inside the budget plus a margin |
| Differential test | `tests/differential/` | everything else a query can do: seeded random tables and queries, run on quarry and DuckDB with random engine options, results compared |
| Flaws matrix | `scripts/run-flaws.sh` | holes in the suite itself (below) |
| Fuzzing | `fuzz/` | crashes, hangs and sanitizer reports on arbitrary bytes: `fuzz_parser` as SQL text, `fuzz_query` as a query over two fixed tables, run with random engine options |
| Sanitizers, coverage | CI jobs `sanitize`, `coverage` | memory errors and undefined behaviour anywhere the tests reach; at least 90% of the lines of `src/` and `tools/` |
| Repository checks | `scripts/check-*.sh` and their tests | include direction between layers, no `arrow::compute` in quarry's objects, English only, no attribution, no absolute paths |

## The differential test

```
uv run tests/differential/run.py --quarry build/release/tools/quarry --seeds 0:300
uv run tests/differential/run.py --quarry build/release/tools/quarry --seed 17 --keep
uv run python -m unittest discover -s tests/differential
```

For each seed, `gen_data.py` writes 2 to 4 Parquet tables of 0 to 2,000 rows to a temporary
directory (every 20 seeds one table is empty and one has a single row; one table in four is sorted
and written with row groups of 50 so that pruning has something to prune), `gen_query.py` makes 5
queries, and `run.py` runs each on quarry (the CLI with `--format json`) and on DuckDB over the same
files. The options of each run are drawn per query: batch size 1, 7 or 2048; unlimited memory or
64 KiB; one optimizer rule switched off, or none. `regression_seeds.txt` lists the seeds that once
exposed a bug; they run first.

The data has NULLs in every column (0 to 30%), keys from small domains so that joins find
duplicates, DECIMAL columns of several scales per table (so that joins and comparisons cross scales),
and extremes: `INTEGER` and `BIGINT` limits, `DECIMAL(18)` and `DECIMAL(25)` near their widest
values, a `DECIMAL(38, s)` column holding values near 10^37 (beyond 64 bits), the earliest and
latest `DATE`, and `DOUBLE` values `k/1024` with `|k| <= 2^30` (exact binary fractions, so sums have
no rounding and no dependence on order).

The queries cover filters with every predicate form (comparisons, `IS [NOT] NULL`, `[NOT] BETWEEN`,
`[NOT] IN` over literals, `AND`, `OR`, `NOT`, DATE arithmetic and intervals on literals), projections
with arithmetic, 1 to 4 tables joined by plain column equalities (`JOIN ... ON` or a comma and
`WHERE`; self joins, several key pairs, cycles), `GROUP BY` with 1 to 3 keys and every aggregate,
`HAVING`, `ORDER BY` and `LIMIT`. `test_generator_coverage.py` counts how often each of these appears
over seeds 0..299 and fails, naming the feature, when any appears fewer than 5 times, so that the
generator cannot decay into producing only simple queries.

### What the generator leaves out, and why

Every query stays inside the subset in [docs/sql.md](sql.md), so that a difference from DuckDB is a
bug in quarry rather than an unsupported feature: aliases only as a bare `ORDER BY` name (`GROUP BY`
and `HAVING` repeat the expression), literal `IN` lists, DATE literals instead of strings, no `SELECT *`,
`DISTINCT`, division, `LIKE`, `CASE`, subqueries or outer joins. `LIMIT` appears only with an
`ORDER BY` over every output column, so the result is unique.

Three rules keep two correct engines from disagreeing about overflow, which depends on which rows
an engine evaluates (DuckDB evaluates the conjuncts of a `WHERE` one after another on the
survivors, and joins in its own order):

- arithmetic uses small columns and literals; columns holding extremes take part in arithmetic only
  in a *plain* query (one table, no `WHERE`), where both engines evaluate every row;
- a comparison is generated only when the common type of its two sides fits 38 digits;
- a `DECIMAL(38)` extreme column is summed only in a plain query, where it cannot be added to
  itself more often than the table has rows.

### How results are compared

`compare.py` states the rules; they follow spec section 12.3.

- Without `ORDER BY` the rows are compared as multisets, with it in order. In an aggregate query the
  group keys come first in the `ORDER BY`, so ties cannot reorder rows that differ only in an AVG.
- Column names are compared case-sensitively and types by name, with one intended exception:
  `SUM` of an `INTEGER` or `BIGINT` is `HUGEINT` in DuckDB and `DECIMAL(38,0)` in quarry, and the two
  are compared by value.
- Integers and decimals are compared by exact value. `DOUBLE` is compared exactly, except an `AVG`
  column, where the relative error may be 1e-12: DuckDB's own `AVG` of a `DECIMAL` varies by one
  ulp between queries on the same group. `-0.0` equals `0.0`, NULL equals NULL.
- Both engines failing counts as agreement only when both raise an overflow-class error (quarry
  `decimal overflow` or `integer overflow`, DuckDB `Out of Range` or a failed cast); the run prints
  the pairs of messages. Any other pair of errors, or one engine failing alone, is a mismatch.
- Under the 64 KiB limit exactly two refusals are by design and are matched by their messages: a
  `Sort` whose input does not fit (a sort needs its whole input in memory), and a `HashJoin`
  partition that still does not fit at the deepest split. Such a query is run again without the
  limit and that run is compared; the summary tallies the re-runs by reason and asserts the set. Any
  other `resource exhausted` under the limit (a hash aggregate that should have spilled, say) is a
  mismatch.
- A query whose `FROM` and `WHERE` return more than 400,000 rows is skipped.

A mismatch prints the seed, the SQL, the engine options and a row diff, and the command that
repeats it. `--keep` leaves the Parquet files in place.

## The flaws matrix

`QUARRY_FLAW=<name>` compiles one deliberate defect into quarry (`src/common/flaws.h`, one
`if constexpr` at the point of the defect). `scripts/run-flaws.sh <name>` builds that variant, runs
the tests that are meant to catch it and succeeds only if every one of them fails; CI runs it once
per flaw. A flaw that gets through means the suite cannot see that kind of bug.

| Flaw | The defect | Caught by |
| --- | --- | --- |
| `selection_off_by_one` | the evaluator gathers one row too far at the end of a selection vector | `EvalProperty_test` |
| `null_as_false` | `NOT NULL` is TRUE | the differential run |
| `decimal_scale_slip` | a decimal rescale by two or more digits moves one digit too few | `Tpch_test` |
| `join_order_drops_predicate` | when a relation is reached by two key pairs, one equality is lost | `JoinOrder_test`, `Tpch_test` |
| `drop_hash_collisions` | a probe gives up at the first occupied slot that is not the key | `JoinHashTable_test`, the differential run |
| `prune_wrong_bound` | row-group pruning treats `<=` as `<` at a group's maximum | `ScanTest.PrunesRowGroupsOnBoundaries`, the differential run |
| `spill_drops_last_batch` | a spill writer does not flush its last rows | `TpchBudget_test` |
| `spill_retains_rows` | spilling keeps a copy of every row in memory, outside the budget | `TpchBudget_test`, `HighCardinalityAgg_test` |

## What the suite does not cover

- Data larger than scale factor 1, and the behaviour of a real machine that runs out of memory:
  spilling is tested within a budget the process can afford, with the memory measured through the
  query's Arrow pool.
- Concurrent queries: an `Engine` runs one query at a time and refuses a second by construction.
- DuckDB's own errors: when the reference is wrong, both engines may be wrong together. The golden
  results were spot-checked against hand-computed values to lessen this.
- Where quarry and DuckDB are known to differ and the comparator allows it: the name of the
  `SUM` type of integers, `DATE + INTERVAL` (DuckDB returns a `TIMESTAMP`), and the text of error
  messages.
- Overflow that depends on the order in which an engine evaluates the rows of a filter or a join,
  for the reason given above: the generator keeps such overflow out of the comparison instead of
  deciding which engine is right.
- Cancellation inside one `Next` call (a long loop that never returns to the batch boundary) and
  the Arrow pool used by the spill reader are known gaps, listed with the other deferred findings.
