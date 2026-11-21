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
| Q1 | 608.4 (603.3-609.8) | 143.0 (140.5-144.3) | 33.4 (32.5-33.6) | 4.25 |
| Q3 | 323.3 (321.6-325.0) | 90.0 (89.7-90.6) | 28.5 (27.5-28.7) | 3.59 |
| Q5 | 577.1 (575.3-587.4) | 94.7 (94.6-95.7) | 29.4 (28.3-29.8) | 6.09 |
| Q6 | 344.3 (342.6-348.7) | 56.4 (55.5-57.7) | 15.7 (15.4-15.9) | 6.10 |

A ratio above 1.00 means quarry is slower than single-threaded DuckDB.

Source: `bench/results/main-20261121T131055Z.json` (commit `479f7227df54`, 2026-11-21T13:10:55Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:main -->

## License

MIT.
