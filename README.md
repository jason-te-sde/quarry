# quarry

A vectorized SQL query engine in C++20 over Apache Arrow and Parquet, checked against DuckDB on
every query in its test suite.

quarry reads Parquet files, plans a `SELECT` statement with a small rule-based and cost-based
optimizer, and runs it on Arrow column batches with its own kernels, hash joins and hash
aggregation. Joins and aggregations spill to disk under a memory limit and return the same answer.
It covers a focused subset of SQL ([the scope, and why](docs/design/0007-scope.md)).

A fix to the join-size estimate took the TPC-H-derived Q5 from <!-- n:before.main.q5.quarry -->4,730<!-- /n --> ms to <!-- n:main.q5.quarry -->590.5<!-- /n --> ms at scale
factor 1, and its peak memory from <!-- n:before.main.q5.rss -->6,288<!-- /n --> MiB to <!-- n:main.q5.rss -->125<!-- /n --> MiB. The
[measured numbers](#measured-numbers) and the [account of where the time goes](#where-the-time-goes)
are below. Every measured number in this file is rendered from the result files in
`bench/results/`, and CI fails when one disagrees.

## Why it is worth a look

Not that it runs: that every answer is checked and every cost is measured.

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
3. **Memory has a limit, and what it covers is measured.** With `--memory-limit`, hash joins and hash
   aggregations that do not fit spill to disk and return the same answer; a sort that does not fit
   stops with an error that says what it needed. The hash tables' storage (key index, key arena,
   aggregate states, join chains) is allocated from the query's memory pool, and a table pays for a
   growth before it allocates it. The pool the operators allocate from stays within the limit plus a margin of 256 KiB <!-- const: the margin the tests assert --> for what the budget charges: for the TPC-H queries under a tiny limit
   ([tests/tpch/tpch_budget_test.cc](tests/tpch/tpch_budget_test.cc)) and for an aggregate with
   600,000 groups and joins at several limits
   ([tests/unit/exec/near_limit_test.cc](tests/unit/exec/near_limit_test.cc), with its measured peaks in
   [ADR 0006](docs/design/0006-memory-and-spill.md#the-margin-and-how-it-is-measured)). What the limit covers and what it does not is under
   [picking a memory limit](docs/operations.md#picking-a-memory-limit).
   Spilling itself is tested in [tests/unit/exec/spill_test.cc](tests/unit/exec/spill_test.cc).
4. **Where the time goes is measured, not guessed.** `--explain-analyze` prints the rows, batches, own
   time and peak state of every operator, a sampling profile is attributed to the operators, and
   [docs/where-time-goes.md](docs/where-time-goes.md) ties each gap to DuckDB to a number. The tables
   are rendered from the committed result files ([bench/render_tables.py](bench/render_tables.py),
   tested by [bench/test_render_tables.py](bench/test_render_tables.py)) and the check that they
   match runs in CI ([scripts/check-readme-numbers.sh](scripts/check-readme-numbers.sh)).
5. **The ablation covers batch size and three of the six rules.** The ablation times batch size and
   switches off `prune_row_groups`, `prune_columns` and `order_joins`, one at a time, as the
   specification asked ([batch size](docs/benchmarks.md#ablation-batch-size),
   [row-group pruning](docs/benchmarks.md#ablation-row-group-pruning),
   [rules](docs/benchmarks.md#ablation-optimizer-rules)). `fold_constants`, `push_filters` and
   `pick_build_side` are switchable too (`--disable-rule`). That every
   rule can be switched off and the four queries still return DuckDB's answers is tested in
   [tests/tpch/tpch_test.cc](tests/tpch/tpch_test.cc); that a switch changes the plan, in
   [tests/unit/planner/rules_test.cc](tests/unit/planner/rules_test.cc) and
   [tests/planner/plan_golden_test.cc](tests/planner/plan_golden_test.cc).

## Try it

You need a C++20 compiler (GCC 14, Clang 18 or Apple Clang), CMake 3.25 and Ninja.

Arrow and Parquet are built once, from the pinned source release and verified against its checksum,
into `~/.local/opt`; the build takes a while. The compiler that builds Arrow is the one that must
build quarry, and the default one may be too old: on Ubuntu 24.04 `c++` is GCC 13, which quarry
refuses after the long Arrow build. Choose the compiler first (the scripts and CMake read `CC` and
`CXX`):

```sh
export CC=gcc-14 CXX=g++-14   # or: export CC=clang-18 CXX=clang++-18
```

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
what every operator did, then Q5 under a 64 KiB limit <!-- const: a parameter of the command below --> so that its joins spill:

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
`quarry::Engine` ([include/quarry/engine.h](include/quarry/engine.h)). Version 0.1.0 has no install
step, no exported CMake package and no `examples/` directory (the specification planned one): to use
the library, add this repository with `add_subdirectory` and link the `quarry_engine` target, as
`tools/` does.

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
  ([tests/tpch](tests/tpch)); the same queries under a 64 KiB limit <!-- const: the budget test's limit -->, spilling, equal them too.
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
| Q1 | 608.3 (603.3-611.5) | 141.0 (140.7-179.7) | 33.3 (33.2-34.4) | 4.31 |
| Q3 | 321.3 (318.8-321.4) | 90.0 (89.6-90.1) | 27.9 (27.7-29.4) | 3.57 |
| Q5 | 590.5 (575.0-610.3) | 93.9 (93.4-94.9) | 29.7 (29.5-30.1) | 6.29 |
| Q6 | 342.4 (342.0-356.3) | 56.6 (55.7-58.6) | 15.8 (15.5-16.0) | 6.05 |

A ratio above 1.00 means quarry is slower than single-threaded DuckDB.

Source: `bench/results/main-20261121T163719Z.json` (commit `2676a9805ba8`, 2026-11-21T16:37:19Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:main -->

Peak memory, the effect of batch size and of row-group pruning, and the earlier results
from before the join-estimate fix (kept under `bench/results/before-join-estimate-fix/`) and before the fixes of the
whole-branch review (`bench/results/before-review-fixes/`, with the comparison) are in
[docs/benchmarks.md](docs/benchmarks.md#results).

What each optimizer rule is worth, with the rule switched off:

<!-- bench:ablation-rules -->
| Rule switched off | Query | quarry, all rules | quarry, rule off |
|---|---|---|---|
| `prune_columns` | Q1 | 619.7 (616.7-635.1) | 1,042 (1,041-1,052) |
| `prune_columns` | Q3 | 318.2 (318.0-357.2) | 1,266 (1,254-1,268) |
| `prune_columns` | Q5 | 577.4 (575.0-595.1) | 1,705 (1,689-1,725) |
| `prune_columns` | Q6 | 347.8 (346.2-349.7) | 868.4 (864.5-882.4) |
| `order_joins` | Q3 | 318.2 (318.0-357.2) | 319.8 (318.3-320.5) |
| `order_joins` | Q5 | 577.4 (575.0-595.1) | 448.8 (444.7-450.5) |

Source: `bench/results/ablation-20261121T164037Z.json` (commit `2676a9805ba8`, 2026-11-21T16:40:37Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:ablation-rules -->

The last row is a loss for the optimizer: with `order_joins` off, Q5 is faster. See below.

And the cost of staying inside a memory limit, with the queries whose joins and aggregations spill:

<!-- bench:spill -->
| Query | Memory limit (MB) | Time (ms) | Spilled (MB) | Operator pool peak (MB) | Peak RSS (MB) |
|---|---|---|---|---|---|
| Q3 | unlimited | 320.5 (319.3-323.4) | 0.00 | 27.20 | 81.95 |
| Q3 | 256MB | 332.7 (329.6-357.8) | 0.00 | 29.12 | 65.91 |
| Q3 | 64MB | 334.5 (329.8-370.7) | 0.00 | 29.12 | 75.58 |
| Q3 | 16MB | 1,954 (1,945-2,038) | 102.98 | 9.68 | 46.92 |
| Q5 | unlimited | 587.8 (582.5-620.4) | 0.00 | 58.76 | 120.01 |
| Q5 | 256MB | 588.8 (586.6-598.8) | 0.00 | 57.05 | 120.82 |
| Q5 | 64MB | 587.5 (584.1-621.8) | 0.00 | 57.05 | 120.90 |
| Q5 | 16MB | 8,124 (7,980-8,213) | 128.18 | 17.09 | 107.99 |
| High-cardinality aggregate | unlimited | 318.2 (315.9-323.8) | 0.00 | 154.14 | 272.94 |
| High-cardinality aggregate | 256MB | 373.0 (340.0-386.3) | 0.00 | 154.14 | 276.61 |
| High-cardinality aggregate | 64MB | 929.5 (923.0-965.9) | 37.54 | 43.03 | 124.62 |
| High-cardinality aggregate | 16MB | 927.4 (882.2-940.5) | 37.55 | 10.79 | 74.50 |

The limit bounds operator state, not the Parquet reader's row-group buffers, so peak RSS stays above it. The operator pool peak is what the query's operators allocated through Arrow's memory pool: the hash tables' storage (key index, key arena, aggregate states, join chains), sort and build-row buffers, spill buffers, batches read back from spill files and expression temporaries. It excludes the Parquet reader's row-group buffers (their own pool), the result table, and the strings that MIN and MAX of a VARCHAR hold (docs/design/0006-memory-and-spill.md). The limit and the three sizes are in the same unit, MB of 10^6 bytes, as `--memory-limit` counts them. A join's build rows are charged once, but a join makes a second copy of them when it concatenates its build side, and that copy can sit in the operator pool, so the pool peak can pass the limit: Q5 at 16MB is above it (known issue in the CHANGELOG). A refused cell is the result of that cell.

Source: `bench/results/spill-20261121T180222Z.json` (commit `2676a9805ba8`, 2026-11-21T18:02:22Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry 256MB High-cardinality aggregate (default data), quarry 16MB High-cardinality aggregate (default data).
<!-- /bench:spill -->

## Where the time goes

Summary of [docs/where-time-goes.md](docs/where-time-goes.md), which has the operator tables, the
sample profile and the method. DuckDB itself was not profiled, so each gap is tied to what quarry
spends its time on, not to what DuckDB does differently.

- **Q1** (ratio <!-- n:main.q1.ratio -->4.31<!-- /n -->): the hash aggregate has <!-- n:ea.q1.2.time -->397.2<!-- /n --> ms of own time, <!-- n:ea.q1.2.share -->62<!-- /n -->% of the operator time, which is more than the
  <!-- n:main.q1.duckdb -->141.0<!-- /n --> ms DuckDB needs for the whole query. There are few groups, so the time is per-row work:
  copying rows, updating sums and checked decimal arithmetic.
- **Q6** (ratio <!-- n:main.q6.ratio -->6.05<!-- /n -->): <!-- n:cls.q6.filter -->78<!-- /n -->% of the samples are the filter pushed into the scan. The query keeps
  <!-- n:ea.q6.2.rows -->114,160<!-- /n --> of <!-- n:data.lineitem_rows -->6,001,215<!-- /n --> rows, so almost all the work is evaluating the predicate for rows that are then
  dropped.
- **Q3** (ratio <!-- n:main.q3.ratio -->3.57<!-- /n -->, the smallest gap): Parquet decode, the hash probe and the pushed-down filters share
  the time, and no single function dominates.
- **Q5** (ratio <!-- n:main.q5.ratio -->6.29<!-- /n -->, the largest gap in milliseconds): <!-- n:cls.q5.probe -->52<!-- /n -->% of the samples are the hash probe, with the
  index lookup alone at <!-- n:frame.q5.KeyIndex::Find.probe -->26.7<!-- /n -->%. It peaks at <!-- n:main.q5.rss -->125<!-- /n --> MiB.

**The Q5 join order is a stated limit.** With `--disable-rule order_joins` Q5 takes
<!-- n:rules.order_joins.off.q5 -->448.8<!-- /n --> ms against <!-- n:rules.on.q5 -->577.4<!-- /n --> ms with the rule on: the optimizer's order is worse than the order
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
- DuckDB's own errors: where the reference is wrong, both engines may be wrong together. One result
  is worked out by hand and asserted in `EngineTest.ResultsWorkedOutByHandAgree`; the golden results
  are DuckDB's own.
- Where quarry and DuckDB are known to differ and the comparator allows it: the type name of `SUM`
  over integers, `DATE + INTERVAL`, and the text of error messages.
- Overflow that depends on the order in which an engine evaluates the rows of a filter or a join:
  the generator keeps it out of the comparison instead of deciding which engine is right.
- Cancellation inside one Parquet row group read or one concatenation copy (the scan checks between
  row groups and batches).

The full list, with the reasons, is in [docs/testing.md](docs/testing.md#what-the-suite-does-not-cover).
What the engine does not do (outer joins, subqueries, `DISTINCT`, `LIKE`, `CASE`, division, threads,
an external sort and the rest) is in [ADR 0007](docs/design/0007-scope.md).

## Project

[CHANGELOG.md](CHANGELOG.md), [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) and
[RELEASING.md](RELEASING.md). Benchmark results are TPC-H-derived and are not TPC-H results (see above).

## License

MIT, see [LICENSE](LICENSE).
