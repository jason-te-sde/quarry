# quarry

A vectorized SQL query engine in C++20 over Apache Arrow and Parquet. Query results are checked
against DuckDB.

Work in progress. The engine runs the four TPC-H queries Q1, Q3, Q5 and Q6 on the SF0.01 test data
and gets DuckDB's answers. Joins and aggregations spill to disk under a memory limit. The numbers
below were measured on SF1, and quarry is slower than DuckDB on every query.

## Build

```sh
scripts/build_arrow.sh            # once: Arrow 26.0.1, pinned and checksum-verified
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
build/dev/tools/quarry schema tests/data/pruning/events.parquet
build/dev/tools/quarry query --data tests/data/tpch-sf0.01 -f tests/tpch/queries/q5.sql --explain-analyze
```

`quarry query --data <dir> (-c SQL | -f FILE)` prints the result as a table, CSV or JSON
(`--format`), or the plan (`--explain`), or the plan with what every operator did
(`--explain-analyze`). `quarry query --help` lists the options.

`uv run scripts/gen_tpch.py` produces the test data and the DuckDB answers;
`uv run scripts/gen_goldens.py` fills in the expected results of `tests/sql/results.test`.
`uv run tests/differential/run.py --quarry build/dev/tools/quarry --seeds 0:300` runs random queries on
random tables against DuckDB; [docs/testing.md](docs/testing.md) describes every layer of the tests.

## Benchmarks

quarry on one thread against DuckDB 2.0.1 on one thread (and, for reference, on all its threads), on
the same Parquet files, TPC-H queries Q1, Q3, Q5 and Q6. The data and the queries are derived from
TPC-H; these are not TPC-H benchmark results and must not be compared with officially published
ones. The protocol is in [docs/benchmarks.md](docs/benchmarks.md), which also holds the ablations
and the cost of spilling; [docs/where-time-goes.md](docs/where-time-goes.md) says where quarry
spends its time and why it loses.

<!-- bench:environment -->
Measured on Apple M2 (8 cores, 16 GB), macOS 26.6.2. quarry: Apple clang version 21.0.0 (clang-2100.3.34.2), Release build, flags `-O3 -DNDEBUG`, Arrow 26.0.1 (static). DuckDB 2.0.1. Data: TPC-H-derived, scale factor 1, Parquet row groups of 122,880 rows (snappy), 308 MiB.
<!-- /bench:environment -->

<!-- bench:main -->
| Query | quarry, 1 thread | DuckDB, threads=1 | DuckDB, default (8 threads) | quarry / DuckDB (threads=1) |
|---|---|---|---|---|
| Q1 | 616.6 (606.4-617.5) | 141.5 (141.1-144.6) | 33.6 (32.8-33.8) | 4.36 |
| Q3 | 319.5 (319.1-322.4) | 90.5 (89.6-91.9) | 27.3 (26.6-28.4) | 3.53 |
| Q5 | 4,730 (4,707-6,062) | 93.8 (93.4-95.1) | 29.2 (28.1-29.3) | 50.40 |
| Q6 | 341.8 (341.6-342.9) | 56.8 (54.6-57.3) | 15.7 (15.2-16.7) | 6.02 |

A ratio above 1.00 means quarry is slower than single-threaded DuckDB.

Source: `bench/results/main-20261121T065519Z.json` (commit `ccb030ca62b7`, 2026-11-21T06:55:19Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every cell.
<!-- /bench:main -->

## License

MIT.
