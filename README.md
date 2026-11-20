# quarry

A vectorized SQL query engine in C++20 over Apache Arrow and Parquet. Query results are checked
against DuckDB.

Work in progress. The engine runs the four TPC-H queries Q1, Q3, Q5 and Q6 on the SF0.01 test data
and gets DuckDB's answers; spilling to disk and the benchmarks are not built yet.

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

## License

MIT.
