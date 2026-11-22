# Changelog

All notable changes to quarry are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/) with the caveat in [RELEASING.md](RELEASING.md): until 1.0,
a minor release may change the SQL it accepts, the library API and the command line.

## [Unreleased]

### Fixed

- Spill directories left by a process that died without cleanup (SIGKILL, a crash, a second Ctrl-C)
  are now removed instead of piling up in the temporary directory: they are named
  `quarry-spill-<pid>-XXXXXX`, and the next query with a memory limit under the same spill parent
  removes the directories of that form whose process no longer exists. Files, symlinks and other
  names are left alone. A spill parent must not be shared between PID namespaces or hosts, which the
  sweep cannot tell apart.

## [0.1.0] - 2026-11-21

The first release.

### Added

- **The engine.** A vectorized SQL query engine in C++20 over Apache Arrow and Parquet, single
  threaded and read-only. A hand-written lexer and recursive-descent parser, a binder that gives every
  expression a type by DuckDB's rules, a rule-based and cost-based optimizer, and pull-based operators
  that process batches of Arrow arrays with a selection vector: Parquet scan with row-group pruning,
  filter, projection, hash join, hash aggregation, sort, top-N and limit. No `arrow::compute` kernel is
  used ([ADR 0003](docs/design/0003-arrow-boundary.md)).
- **SQL**: `SELECT` with aliases, `FROM` with comma joins and `[INNER] JOIN ... ON`, `WHERE`,
  `GROUP BY`, `HAVING`, `ORDER BY` (several keys, `ASC`/`DESC`, `NULLS FIRST`/`LAST`), `LIMIT`;
  `SUM`, `AVG`, `COUNT`, `COUNT(*)`, `MIN`, `MAX`; arithmetic, comparisons, `AND`/`OR`/`NOT`,
  `BETWEEN`, `IN` over constants and `IS [NOT] NULL` with SQL's three-valued logic; `DATE` literals
  with `INTERVAL` arithmetic on constants; the types `BOOLEAN`, `INTEGER`, `BIGINT`, `DECIMAL(p, s)` up
  to 38 digits, `DOUBLE`, `DATE` and `VARCHAR`. Errors name the line and column
  ([docs/sql.md](docs/sql.md)). What is left out, and why, is
  [ADR 0007](docs/design/0007-scope.md).
- **The optimizer** ([ADR 0005](docs/design/0005-optimizer.md)): constant folding, filter push-down
  into the scans, column pruning, row-group pruning from the Parquet min/max, a greedy join order
  chosen from the join graph, and the choice of the build side. Each rule except join construction
  can be switched off with `--disable-rule`.
- **A memory budget and spilling** ([ADR 0006](docs/design/0006-memory-and-spill.md),
  [operations](docs/operations.md)): hash joins and hash aggregations that do not fit the limit spill
  to Arrow IPC files in a directory that belongs to the query and is removed however the query ends;
  a sort that does not fit stops with `resource exhausted`.
- **The interfaces**: the library class `quarry::Engine`
  ([include/quarry/engine.h](include/quarry/engine.h)), and the `quarry` command line with
  `query`, `schema`, `--explain`, `--explain-analyze` (rows, batches, own time, peak state and spilled
  bytes of every operator), `--format table|csv|json`, `--memory-limit`, `--batch-size`,
  `--disable-rule` and `--timeout`. Ctrl-C ends a query and removes its spill files.
- **Correctness evidence** ([docs/testing.md](docs/testing.md)): unit and property tests, SQL tests
  with results generated from DuckDB, golden plans, the four TPC-H-derived queries against DuckDB's
  answers (also under a tiny memory limit), a seeded differential test of random queries against
  DuckDB, a matrix of deliberately injected flaws that each must turn the suite red, fuzz targets for
  the parser and for whole queries, sanitizers and a line-coverage floor.
- **Measurements** ([docs/benchmarks.md](docs/benchmarks.md),
  [docs/where-time-goes.md](docs/where-time-goes.md)): a protocol committed before the first
  measurement, results against single-threaded and multi-threaded DuckDB on TPC-H-derived data at scale
  factor 1, ablations of batch size, pruning and the optimizer rules, the cost of spilling, and a
  sampled profile. quarry is slower than single-threaded DuckDB on all four queries; the README says by
  how much, rendered from the result files, and CI fails when it disagrees with them. Join order on Q5
  is still worse than the order the query is written in, a limit of the fixed selectivities that
  ADR 0005 describes.
- **Documentation**: [README](README.md), [architecture](docs/architecture.md), the
  [SQL reference](docs/sql.md), [operations](docs/operations.md), [testing](docs/testing.md), and
  seven [design records](docs/design/).
- **Release machinery**: a release workflow that refuses a tag that is not the version in
  `CMakeLists.txt`, [RELEASING.md](RELEASING.md), `scripts/preflight.sh` and a CI job that follows the
  README on a fresh checkout.

### Fixed

- Before the first release, the join-size estimate treated a join of two foreign keys to the same
  table as if one were a primary key, which made the greedy order build a very large hash table on
  Q5. A key pair's divisor is now capped at the value range read from the Parquet min/max. The earlier
  results are kept under `bench/results/before-join-estimate-fix/`.
- A scan read the wrong Parquet column after a `STRUCT`, `MAP` or other nested column: it passed Arrow
  field positions to the reader, which takes leaf columns, so `SELECT y` could return another column's
  values without an error. Fields now map to leaves through the schema manifest, row-group statistics
  use the same mapping (so pruning works on such files), and selecting the nested column itself is still
  refused.
- A scan whose filter matches nothing could not be interrupted: Ctrl-C and `--timeout` were checked
  only when an operator returned a batch, so the scan read its whole input first, and a query that
  outlived its timeout this way succeeded. The scan now checks once per row group and per batch it
  reads. A cancel that arrived while a query was being planned was lost; it now ends the query, and a
  second Ctrl-C ends the `quarry` process.
- `AVG` and `COUNT` per group were quadratic in the number of groups: every output batch recomputed
  every group. Producing the output is now linear.
- The memory limit measured less than it said. The hash tables' storage (key index, key arena,
  aggregate states, join chains) is now allocated from the query's memory pool, so `pool_peak_bytes`
  and the README's operator-pool column include it (the high-cardinality aggregate used to show
  none), and a table pays for a growth, such as the doubled slot array of a rehash, before it
  allocates it. Batches read back from spill files are allocated from the pool as well. What the limit
  still does not cover is in [docs/operations.md](docs/operations.md): one row group per scan, and the
  second copy that exists while a join or a sort concatenates its input.
- A column left in several chunks after a row group was read is an error that names the file, not an
  empty array; an unreadable data page names its file too.

### Known issues

Found before this release and not fixed in it; each is small, and none returns a wrong answer.

- **A join's pool can pass the memory limit.** The operators' pool stays within the limit plus
  256 KiB for the state the budget charges, but not for a join's build side. `ConcatenateBuild` copies
  the build rows into one array per column, and the budget charges one copy. The second copy sits in
  the operator pool when the build input is filtered, is another join's or aggregate's output, or is
  read back from a spill file, so the pool can pass the limit by up to the payload of the build
  partition in memory, and above 2^20 build rows by up to twice that. Measured: Q5 at
  `--memory-limit 16MB` has a pool peak of 17,091,680 bytes against a limit of 16,000,000 and a bound of
  16,262,144. A near-limit test with a filtered build side pins the overshoot at one build payload. The
  fix is to charge the concatenation before it is made and spill or split when that is refused, or to
  release the parts as they are copied. The answers are right at every limit.
- **Limits that are documented.** A sort that does not fit its limit stops with `resource exhausted`
  (there is no external sort). A sort holds its input twice in the operator pool while it concatenates
  it, so its pool peak can reach twice the limit. The strings that `MIN` and `MAX` of a `VARCHAR` keep
  are charged but not allocated from the pool. A 257-term `AND` chain is a syntax error. `AND` and
  `OR` evaluate both sides, so `FALSE AND <overflow>` is an error. Comparing a `DECIMAL(38,0)` with a
  `DECIMAL(38,10)` can raise an overflow on legitimate values. Several joins that spill share one
  budget, so an inner join can fail at depth 3 although it would fit alone.
- **Performance.** Literals are broadcast to a full column and `IN` over constants is folded into
  equalities, which is visible in the Q6 profile. The Q5 join order is worse than the order the query
  is written in (see the README).
- **Error handling in rarely used paths.** An aggregate whose state cannot be made is reported from
  the first `Update`, not at construction; `Run` skips `Close` when `Open` fails (the destructors
  release everything); an empty `VARCHAR` offsets buffer is dereferenced where operators never pass one;
  `ValueOrDie` is used when a result string passes 2 GiB; `AddDirectory` is not atomic and registers a
  directory with no Parquet file silently; destructors swallow the errors of removing spill files.
- **Gaps in the tests.** Nothing fails if the engine stops forgetting a `Cancel()` that came before
  `Execute()`, or stops checking the cancel flag during planning (the fix is in `Engine::Execute` and
  the command line's signal handler, and no test pins it); the hygiene test does not pin every
  fail-closed branch; there is no test of a
  nested join at depth 3, of Top-N spilling behaviour, of file descriptors, of a `DECIMAL` comparison
  at 2^63 and above, or of `AddInterval` across centuries; the differential test has no floor on the
  number of non-empty compared results; unreadable-file cases skip when run as root; the object check
  scans the libraries and not the test executables or the binary; the SF1 Q5 join order is not pinned
  by a test (the SF0.01 golden and a synthetic test pin the estimator).
- **Tooling.** `scripts/gen_tpch.py` assumes `DECIMAL(p, s)` columns; the lint job builds Arrow with
  Clang 18 only for clang-tidy; the test scripts are mode 100644 and run through `bash`;
  `AddressSanitizer` on macOS stops inside libarrow and libparquet (see
  [docs/testing.md](docs/testing.md)); `check_answers` of the benchmark harness does not record in the
  result that every variant's answer was checked.


[Unreleased]: https://github.com/jason-te-sde/quarry/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jason-te-sde/quarry/releases/tag/v0.1.0
