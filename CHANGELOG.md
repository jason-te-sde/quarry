# Changelog

All notable changes to quarry are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/) with the caveat in [RELEASING.md](RELEASING.md): until 1.0,
a minor release may change the SQL it accepts, the library API and the command line.

## [Unreleased]

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

[Unreleased]: https://github.com/jason-te-sde/quarry/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jason-te-sde/quarry/releases/tag/v0.1.0
