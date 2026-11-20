# quarry

A vectorized SQL query engine in C++20 over Apache Arrow and Parquet. Query results are checked
against DuckDB.

Work in progress: this is the scaffold. The engine is not built yet.

## Build

```sh
scripts/build_arrow.sh            # once: Arrow 26.0.1, pinned and checksum-verified
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
build/dev/tools/quarry schema tests/data/pruning/events.parquet
```

`uv run scripts/gen_tpch.py` produces the test data and the DuckDB answers.

## License

MIT.
