# ADR 0003: The Arrow boundary

Status: accepted.

## Decision

quarry uses Apache Arrow for three things and nothing else:

- the **memory format**: columnar arrays, record batches, schemas and scalars are `arrow::Array`,
  `arrow::RecordBatch`, `arrow::Schema` and `arrow::Scalar`;
- the **Parquet reader**: footers, row-group statistics and column chunks come from
  `parquet::ParquetFileReader` and `parquet::arrow::FileReader`;
- **IPC**, for the spill files and result exchange.

quarry does **not** use Arrow's compute layer. Every expression kernel, hash table, aggregation,
join and sort is quarry's own code, so that what the benchmarks time is the engine and not a
library call. DuckDB, which the results are checked against, is a separate program in Python
tooling only; it is never linked.

## Enforcement

`scripts/check-layering.sh` runs in CI (job `lint`) and fails on:

- any `#include <arrow/compute/...>` under `src/` or `include/`;
- any reference to the `arrow::compute` namespace in code under `src/` or `include/`;
- any Arrow or Parquet header in `src/parser`.

The same script checks the include direction between the directories of `src/` (the link graph of
`src/CMakeLists.txt`), and that library code neither writes to stdout or stderr nor throws.
`nm` on quarry's own objects shows no `arrow::compute` symbol.

## The pinned build

Arrow and Parquet 26.0.1 are built by `scripts/build_arrow.sh` from `apache-arrow-26.0.1.tar.gz`,
verified against the `.sha512` file published beside it (the build fails on a mismatch or a missing
checksum). No system or Homebrew Arrow is ever used. The install prefix defaults to
`$HOME/.local/opt/arrow-26.0.1-<compiler-id>`; `cmake -DQUARRY_ARROW_PREFIX=<dir>` overrides it.
`cmake/QuarryArrow.cmake` finds it with `find_package(Arrow 26.0.1 EXACT)` and `find_package(Parquet)`.

Flags: `-DCMAKE_BUILD_TYPE=Release -DARROW_BUILD_STATIC=ON -DARROW_BUILD_SHARED=OFF
-DARROW_PARQUET=ON -DARROW_IPC=ON -DARROW_WITH_SNAPPY=ON -DARROW_WITH_ZSTD=ON -DARROW_COMPUTE=OFF
-DARROW_CSV=OFF -DARROW_JSON=OFF -DARROW_DATASET=OFF -DARROW_FILESYSTEM=OFF -DARROW_JEMALLOC=OFF
-DARROW_MIMALLOC=OFF -DARROW_DEPENDENCY_SOURCE=BUNDLED`.

Installed libraries (`<prefix>/quarry-arrow-build.txt` lists them for each build):
`libarrow.a`, `libparquet.a` and `libarrow_bundled_dependencies.a` (Thrift, Snappy and Zstd).

Measured on an Apple M2 with Apple clang 21 and 4 parallel jobs: the build took 113 s and the
install prefix is 37 MB (`du -sh`). The build and source directories are deleted afterwards.
<!-- obs: one build, timed by hand, size read with `du -sh`; not a benchmark and no results file holds it -->

### Components Parquet forces on

Configuring did not fail, and no flag had to be changed. But `ARROW_PARQUET=ON` makes the build
compile Arrow's compute kernels into `libarrow.a` even with `-DARROW_COMPUTE=OFF`: the library
defines `arrow::compute` symbols and the install carries `arrow/compute/` headers, which Parquet's
own reader uses internally. The ban above is therefore on quarry's use of the compute layer, not on
its presence in the archive, and it is the reason the ban is enforced by a script rather than by the
build configuration. The static link pulls into a binary only the compute objects Parquet needs.

## Types the Parquet reader returns

A `DECIMAL(12,2)` column is read as `decimal128(12, 2)`; Arrow 26.0.1's Parquet schema conversion
does not narrow a decimal to `decimal64` or `decimal32`. Dates are `date32[day]` and 64-bit
integers `int64`. Statistics for those columns convert to scalars of the same Arrow type
(`Date32Scalar`, and so on), which is what row-group pruning compares against.
