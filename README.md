# quarry

A vectorized SQL query engine in C++20 over Apache Arrow and Parquet, checked against DuckDB on
every query it runs.

quarry reads Parquet files, plans a `SELECT` statement with a small rule-based and cost-based
optimizer, and runs it on Arrow column batches with its own kernels, hash joins and hash
aggregation. Joins and aggregations spill to disk under a memory limit. The engine is single
threaded, read-only and runs a deliberate subset of SQL ([what is left out, and why](docs/design/0007-scope.md)).

**It is slower than DuckDB.** On the TPC-H-derived queries Q1, Q3, Q5 and Q6 at scale factor 1,
single-threaded quarry takes a multiple of single-threaded DuckDB's time on every one of them: the
ratios are Q1 <!-- n:main.q1.ratio -->4.25<!-- /n -->, Q3 <!-- n:main.q3.ratio -->3.59<!-- /n -->, Q5 <!-- n:main.q5.ratio -->6.09<!-- /n --> and Q6 <!-- n:main.q6.ratio -->6.10<!-- /n -->. Q5 was
<!-- n:before.main.q5.ratio -->50.40<!-- /n -->x before a fix to the join-size estimate, and its join order is still not the best one. The
[measured numbers](#measured-numbers) and the [account of where the time goes](#where-the-time-goes)
are below. Every measured number in this file is rendered from the result files in
`bench/results/`, and CI fails when one disagrees.

## Why it is worth a look

Not that it runs: that it is checked, and that its slowness is explained.

1. **Every result is compared with DuckDB's.** The four queries are checked cell by cell against
   DuckDB's answers on the committed test data ([tests/tpch/tpch_test.cc](tests/tpch/tpch_test.cc)),
   and so are the SQL tests whose expected results were generated from DuckDB
   ([tests/sql/results.test](tests/sql/results.test)). A seeded generator of random tables and random
   queries (filters, joins, aggregates, NULLs, extreme values, row groups to prune) runs each query on
   both engines with random engine options and compares the results
   ([tests/differential](tests/differential), run on every push and, with many more seeds, nightly by
   [nightly.yml](.github/workflows/nightly.yml)).
2. **The test suite can turn red.** Each of a set of deliberately injected defects (a selection vector
   off by one, NULL treated as false, a decimal rescale one digit short, a hash probe that gives up at
   a collision, a spill that loses its last batch, ...) is compiled in and must make the tests fail;
   a CI job per defect asserts it ([scripts/run-flaws.sh](scripts/run-flaws.sh), the table in
   [docs/testing.md](docs/testing.md#the-flaws-matrix)).
3. **Memory has a limit.** With `--memory-limit`, hash joins and hash aggregations that do not fit
   spill to disk and return the same answer; a sort that does not fit stops with an error that says
   what it needed. The TPC-H queries are run again under a limit far below their needs and the
   memory the operators allocate is measured against the limit plus a margin
   ([tests/tpch/tpch_budget_test.cc](tests/tpch/tpch_budget_test.cc),
   [tests/unit/exec/spill_test.cc](tests/unit/exec/spill_test.cc),
   [ADR 0006](docs/design/0006-memory-and-spill.md)).
4. **Where the time goes is measured, not guessed.** `--explain-analyze` prints the rows, batches, own
   time and peak state of every operator, a sampling profile is attributed to the operators, and
   [docs/where-time-goes.md](docs/where-time-goes.md) ties each gap to DuckDB to a number. The tables
   are rendered from the committed result files ([bench/render_tables.py](bench/render_tables.py),
   tested by [bench/test_render_tables.py](bench/test_render_tables.py)) and the check that they
   match runs in CI ([scripts/check-readme-numbers.sh](scripts/check-readme-numbers.sh)).
5. **Every optimizer rule has an ablation.** Batch size, column pruning, row-group pruning and join
   ordering are each switched off and timed ([ablation tables](docs/benchmarks.md#ablation-batch-size)),
   and each rule is switchable on the command line (`--disable-rule`) because a test needs to run the
   plan without it ([tests/unit/planner/rules_test.cc](tests/unit/planner/rules_test.cc),
   [tests/planner/plan_golden_test.cc](tests/planner/plan_golden_test.cc)).

## Try it

You need a C++20 compiler (GCC 14, Clang 18 or Apple Clang), CMake 3.25 and Ninja.

Arrow and Parquet are built once, from the pinned source release and verified against its checksum,
into `~/.local/opt`; the build takes a while:

```sh
scripts/build_arrow.sh
```

Build quarry:

<!-- run -->
```sh
cmake --preset release
cmake --build --preset release
```

The repository holds TPC-H data at scale factor 0.01 in `tests/data/tpch-sf0.01`. Run Q1, then Q3 with
what every operator did, then Q5 under a 64 KiB limit so that its joins spill:

<!-- run -->
```sh
build/release/tools/quarry schema tests/data/pruning/events.parquet
build/release/tools/quarry query --data tests/data/tpch-sf0.01 -f tests/tpch/queries/q1.sql
build/release/tools/quarry query --data tests/data/tpch-sf0.01 -f tests/tpch/queries/q3.sql --explain-analyze
build/release/tools/quarry query --data tests/data/tpch-sf0.01 -f tests/tpch/queries/q5.sql --memory-limit 64KiB --explain-analyze
```

`quarry query --data <dir> (-c SQL | -f FILE)` prints the result as a table, CSV or JSON
(`--format`), or the plan (`--explain`), or the plan with what every operator did
(`--explain-analyze`). `--memory-limit`, `--batch-size`, `--disable-rule` and `--timeout` set the
engine's options; `quarry query --help` lists them all. The library is one class,
`quarry::Engine` ([include/quarry/engine.h](include/quarry/engine.h)).

Check the engine against DuckDB yourself. This runs a short range of random seeds on quarry and on
the DuckDB that `uv.lock` pins, and needs [uv](https://docs.astral.sh/uv/):

<!-- run -->
```sh
uv run --locked tests/differential/run.py --quarry build/release/tools/quarry --seeds 0:20
```

The whole test suite is `ctest --preset release` after a release build; `uv run scripts/gen_tpch.py`
regenerates the test data and DuckDB's answers, and `uv run scripts/gen_goldens.py` the expected
results of `tests/sql/results.test`. [docs/testing.md](docs/testing.md) describes every layer of the
tests.

## How it works

```
 SQL text
    |  lexer, recursive-descent parser                  src/parser
    v
 syntax tree
    |  binder: names, types, DuckDB's decimal rules     src/binder   (the catalog holds Parquet footers)
    v
 logical plan
    |  fold constants, push filters, order joins,       src/planner
    |  pick build sides, prune columns and row groups
    v
 physical plan  <--- EXPLAIN
    |  one operator per node, each pulled by its parent src/exec
    v
 Scan -> Filter -> HashJoin -> HashAggregate -> Sort/TopN    batches of Arrow arrays plus a
    ^                  |                                      selection vector (src/vector, src/expr)
    |                  +--- spill files (Arrow IPC) under a memory budget (src/io)
 Parquet files (Arrow reader: row-group min/max for pruning)
```

[docs/architecture.md](docs/architecture.md) traces one query (Q3) through every layer with a link to
the code and the test of each step. The design decisions are in [docs/design](docs/design): why
vectorized interpretation ([0001](docs/design/0001-vectorized-interpretation.md)) and pull-based
batches ([0002](docs/design/0002-pull-based-batches.md)), what is taken from Arrow and what is not
([0003](docs/design/0003-arrow-boundary.md)), the decimal and type rules
([0004](docs/design/0004-decimal-and-type-rules.md)), the optimizer
([0005](docs/design/0005-optimizer.md)), memory and spilling
([0006](docs/design/0006-memory-and-spill.md)), and the scope ([0007](docs/design/0007-scope.md)). The
accepted SQL is in [docs/sql.md](docs/sql.md), and how to choose a memory limit and read its errors is in
[docs/operations.md](docs/operations.md).

## Correctness

- **Answers.** The four queries at scale factor 0.01 equal DuckDB's, with decimals digit for digit
  ([tests/tpch](tests/tpch)); the same queries under a 64 KiB limit, spilling, equal them too.
- **Plans.** The plan chosen for each query is a golden file, so an optimizer change that alters a plan
  nobody meant to alter fails a test ([tests/planner/plans](tests/planner/plans)).
- **Random queries.** The [differential test](docs/testing.md#the-differential-test) runs seeded random
  queries on random tables on quarry and DuckDB. Its generator is tested to keep producing every
  feature, so it cannot decay into simple queries; the rules for comparing results (and the one
  intended difference, the type name of `SUM` over integers) are written down.
- **A suite that can fail.** The [flaws matrix](docs/testing.md#the-flaws-matrix): every injected
  defect must be caught.
- **Hostile input.** libFuzzer targets for the parser and for whole queries
  ([fuzz](fuzz)) run in CI under AddressSanitizer and UndefinedBehaviorSanitizer, and the whole suite
  runs under both.
- **Repository rules.** Include direction between layers, no `arrow::compute` in quarry's own code
  or objects, English only, no absolute paths ([scripts](scripts) and
  [tests/scripts](tests/scripts)).

## Measured numbers

quarry on one thread against DuckDB on one thread (and, for reference, on all its threads), on the
same Parquet files, TPC-H queries Q1, Q3, Q5 and Q6. The data and the queries are derived from
TPC-H; these are not TPC-H benchmark results and must not be compared with officially published
ones. The protocol is in [docs/benchmarks.md](docs/benchmarks.md). It was committed before the first
measurement and the benchmark script refuses to write a result from a tree with uncommitted changes.

<!-- bench:environment -->
Measured on Apple M2 (8 cores, 16 GB), macOS 26.6.2. quarry: Apple clang version 21.0.0 (clang-2100.3.34.2), Release build, flags `-O3 -DNDEBUG`, Arrow 26.0.1 (static). DuckDB 2.0.1. Data: TPC-H-derived, scale factor 1, Parquet row groups of 122,880 rows (snappy), 308 MiB.
<!-- /bench:environment -->

<!-- bench:main -->
| Query | quarry, 1 thread | DuckDB, threads=1 | DuckDB, default (8 threads) | quarry / DuckDB (threads=1) |
|---|---|---|---|---|
| Q1 | 608.4 (603.3-609.8) | 143.0 (140.5-144.3) | 33.4 (32.5-33.6) | 4.25 |
| Q3 | 323.3 (321.6-325.0) | 90.0 (89.7-90.6) | 28.5 (27.5-28.7) | 3.59 |
| Q5 | 577.1 (575.3-587.4) | 94.7 (94.6-95.7) | 29.4 (28.3-29.8) | 6.09 |
| Q6 | 344.3 (342.6-348.7) | 56.4 (55.5-57.7) | 15.7 (15.4-15.9) | 6.10 |

A ratio above 1.00 means quarry is slower than single-threaded DuckDB.

Source: `bench/results/main-20261121T131055Z.json` (commit `479f7227df54`, 2026-11-21T13:10:55Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:main -->

quarry is slower than DuckDB on all four queries even when DuckDB is held to one thread, and further
behind when DuckDB uses every thread. Peak memory, the effect of batch size and of row-group pruning, and the earlier results
from before the join-estimate fix (kept under `bench/results/before-join-estimate-fix/`) are in
[docs/benchmarks.md](docs/benchmarks.md#results).

What each optimizer rule is worth, with the rule switched off:

<!-- bench:ablation-rules -->
| Rule switched off | Query | quarry, all rules | quarry, rule off |
|---|---|---|---|
| `prune_columns` | Q1 | 618.4 (617.9-630.3) | 1,044 (1,043-1,048) |
| `prune_columns` | Q3 | 319.6 (318.4-320.9) | 1,261 (1,259-1,272) |
| `prune_columns` | Q5 | 573.2 (572.9-602.5) | 1,704 (1,683-1,708) |
| `prune_columns` | Q6 | 343.1 (342.4-383.4) | 870.4 (869.1-882.6) |
| `order_joins` | Q3 | 319.6 (318.4-320.9) | 319.7 (318.8-323.4) |
| `order_joins` | Q5 | 573.2 (572.9-602.5) | 447.0 (446.2-475.4) |

Source: `bench/results/ablation-20261121T131139Z.json` (commit `479f7227df54`, 2026-11-21T13:11:39Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:ablation-rules -->

The last row is a loss for the optimizer: with `order_joins` off, Q5 is faster. See below.

And the cost of staying inside a memory limit, with the queries whose joins and aggregations spill:

<!-- bench:spill -->
| Query | Memory limit | Time (ms) | Spilled (MiB) | Operator state peak (MiB) | Peak RSS (MiB) |
|---|---|---|---|---|---|
| Q3 | unlimited | 323.2 (322.5-334.0) | 0 | 13 | 65 |
| Q3 | 256MB | 334.3 (332.7-339.3) | 0 | 13 | 66 |
| Q3 | 64MB | 334.2 (334.0-334.5) | 0 | 13 | 66 |
| Q3 | 16MB | 1,610 (1,600-1,612) | 98 | 6 | 45 |
| Q5 | unlimited | 589.5 (585.2-597.2) | 0 | 34 | 121 |
| Q5 | 256MB | 596.2 (595.5-603.8) | 0 | 34 | 114 |
| Q5 | 64MB | 596.0 (594.6-607.4) | 0 | 34 | 115 |
| Q5 | 16MB | 7,375 (7,366-7,408) | 122 | 11 | 111 |
| High-cardinality aggregate | unlimited | 302.0 (300.7-307.4) | 0 | 0 | 271 |
| High-cardinality aggregate | 256MB | 322.0 (321.0-326.1) | 0 | 0 | 265 |
| High-cardinality aggregate | 64MB | 758.8 (754.7-783.0) | 36 | 0 | 165 |
| High-cardinality aggregate | 16MB | 725.6 (714.1-751.8) | 36 | 0 | 84 |

The limit bounds operator state only, not the Parquet reader's row-group buffers, so peak RSS stays above it. Batches read back from spill files use Arrow's default pool and are outside the operator-state peak (docs/design/0006-memory-and-spill.md). A refused cell is the result of that cell.

Source: `bench/results/spill-20261121T135405Z.json` (commit `479f7227df54`, 2026-11-21T13:54:05Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:spill -->

## Where the time goes

Summary of [docs/where-time-goes.md](docs/where-time-goes.md), which has the operator tables, the
sample profile and the method. DuckDB itself was not profiled, so each gap is tied to what quarry
spends its time on, not to what DuckDB does differently.

- **Q1** (ratio <!-- n:main.q1.ratio -->4.25<!-- /n -->): the hash aggregate has <!-- n:ea.q1.2.time -->391.9<!-- /n --> ms of own time, <!-- n:ea.q1.2.share -->64<!-- /n -->% of the operator time, which is more than the
  <!-- n:main.q1.duckdb -->143.0<!-- /n --> ms DuckDB needs for the whole query. There are few groups, so the time is per-row work:
  copying rows, updating sums, checked decimal arithmetic and hashing an encoded key for every row.
- **Q6** (ratio <!-- n:main.q6.ratio -->6.10<!-- /n -->): <!-- n:cls.q6.filter -->78<!-- /n -->% of the samples are the filter pushed into the scan. The query keeps
  <!-- n:ea.q6.2.rows -->114,160<!-- /n --> of <!-- n:ea.q5.7.rows -->6,001,215<!-- /n --> rows, so almost all the work is evaluating the predicate for rows that are then
  dropped.
- **Q3** (ratio <!-- n:main.q3.ratio -->3.59<!-- /n -->, the smallest gap): Parquet decode, the hash probe and the pushed-down filters share
  the time, and no single function dominates.
- **Q5** (ratio <!-- n:main.q5.ratio -->6.09<!-- /n -->, the largest gap in milliseconds): <!-- n:cls.q5.probe -->54<!-- /n -->% of the samples are the hash probe, with the
  index lookup alone at <!-- n:frame.q5.KeyIndex::Find.probe -->28.1<!-- /n -->%. It peaks at <!-- n:main.q5.rss -->114<!-- /n --> MiB.

**The Q5 join order is a stated limit.** With `--disable-rule order_joins` Q5 takes
<!-- n:rules.order_joins.off.q5 -->447.0<!-- /n --> ms against <!-- n:rules.on.q5 -->573.2<!-- /n --> ms with the rule on: the optimizer's order is worse than the order
the query was written in. The estimator has no statistics beyond row counts and the minimum and maximum of
each column per row group, plus fixed selectivities, and a fixed selectivity for `r_name = 'ASIA'`
(where one region in five matches) is the likely reason; that reason was not tested. The estimator's
known failure cases are in [ADR 0005](docs/design/0005-optimizer.md).

**What was fixed.** Before the join-estimate fix Q5 took <!-- n:before.main.q5.quarry -->4,730<!-- /n --> ms (<!-- n:before.main.q5.ratio -->50.40<!-- /n -->x DuckDB) and peaked at
<!-- n:before.main.q5.rss -->6,288<!-- /n --> MiB, because the estimate treated the join of two foreign keys as if one were a primary key
and the greedy order built a hash table of <!-- n:ea.q5before.5.rows -->18,435,748<!-- /n --> rows. A key pair's divisor is now capped at the
value range read from the Parquet min/max. The earlier results stay in the repository, and the
suites were re-run after the fix.

## What the test suite does not cover

- Data larger than scale factor 1, and the behaviour of a real machine that runs out of memory.
  Spilling is tested within a budget the process can afford.
- Concurrent queries: an `Engine` runs one query at a time and refuses a second by construction.
- DuckDB's own errors: where the reference is wrong, both engines may be wrong together. The golden
  results were spot-checked against hand-computed values to lessen this.
- Where quarry and DuckDB are known to differ and the comparator allows it: the type name of `SUM`
  over integers, `DATE + INTERVAL`, and the text of error messages.
- Overflow that depends on the order in which an engine evaluates the rows of a filter or a join:
  the generator keeps it out of the comparison instead of deciding which engine is right.
- Cancellation inside one `Next` call that never returns to a batch boundary.

The full list, with the reasons, is in [docs/testing.md](docs/testing.md#what-the-suite-does-not-cover).
What the engine does not do (outer joins, subqueries, `DISTINCT`, `LIKE`, `CASE`, division, threads,
an external sort and the rest) is in [ADR 0007](docs/design/0007-scope.md).

## Project

[CHANGELOG.md](CHANGELOG.md), [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) and
[RELEASING.md](RELEASING.md). Benchmark results are TPC-H-derived and are not TPC-H results (see above).

## License

MIT, see [LICENSE](LICENSE).
