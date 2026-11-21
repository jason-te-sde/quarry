# Benchmark protocol

This file is the protocol for every number quarry publishes. It was committed before the first
SF1 measurement existed (`git log --follow docs/benchmarks.md` shows the order), and the benchmark
script refuses to write a result while this file, the script or anything else in the tree has an
uncommitted change. If a result looks bad, the protocol does not change to fix it; the result
stays and says so.

The queries are TPC-H Q1, Q3, Q5 and Q6 as committed in `tests/tpch/queries/`. The data is derived
from TPC-H. These are not TPC-H benchmark results, they were not produced under the TPC's
benchmark rules, and they must not be compared with officially published TPC-H results.

## What is compared

quarry (this repository, one thread) against DuckDB 2.0.1 (the version pinned in `uv.lock`), run
two ways:

- DuckDB with `threads=1`: the primary comparison, one core against one core.
- DuckDB with its default thread count: for reference only. It is not a fair match for a
  single-threaded engine and is never used to claim a win or to excuse a loss.

## Machine

An Apple M2 (8 cores), 16 GB of memory, macOS. Each result file records the model, the core count,
the memory, the macOS version, the compiler and its version, the compiler flags, and the Arrow
version and Arrow build flags (read from `quarry-arrow-build.txt` in the Arrow install). quarry is
a Release build, `-O3 -DNDEBUG`, without `-march=native`: the binary does not depend on the CPU it
was built on. Arrow is a static Release build without the compute kernels, without jemalloc and
without mimalloc (the system allocator).

## Data

DuckDB's `tpch` extension generates SF1 (`CALL dbgen(sf = 1)`) and DuckDB exports every table to
Parquet with its default row-group size (122,880 rows), in dbgen's original row order. There are two
exports, made by `scripts/gen_tpch.py`:

- `bench/data/sf1`: the default export. All comparisons use it unless a row says otherwise.
- `bench/data/sf1-sorted`: the same, with `lineitem` ordered by `l_shipdate`. It is used only for
  the row-group pruning ablation, because dbgen's order leaves the dates in almost every row group
  and min/max pruning has little to remove there. The sorted file helps both engines in the same
  way.

Both engines read the same files. DuckDB sees each file through a view over `read_parquet`; it does
not import the data into a native table. The result files record the scale factor, the row-group
size and whether the export is sorted.

## Threads

quarry runs on one thread. DuckDB runs with `threads=1` and, separately, with its default.

## Timing boundary

Each run is timed from the moment the SQL text is in hand to the moment the whole result is
materialised in memory as an Arrow table. Parsing, binding, planning and optimisation are inside
the timing; process start, registering the files and printing are outside it. The clock is a
monotonic clock (`std::chrono::steady_clock` in quarry, `time.perf_counter_ns` in the DuckDB
worker). DuckDB's result is materialised with `to_arrow_table()`.

## Process model and repetitions

Every (engine, configuration, query) cell runs in a fresh process. Inside that process: one warm-up
execution, then five measured executions, one after another, and the process exits. The result
reports the median, the minimum and the maximum of the five measured runs, in milliseconds, and
keeps all five. The warm-up is never part of a statistic.

Each cell also reports peak resident set size (`getrusage` `ru_maxrss`) for the whole process.
quarry also reports the operator-state peak and the scan-buffer peak from its own memory pools
(`pool_peak_bytes` and `scan_pool_peak_bytes`, the same two numbers `--explain-analyze` prints).
The DuckDB figure includes the Python interpreter, the `duckdb` and `pyarrow` modules and the
connection, so each DuckDB worker also reports its RSS just before its first query, and the
tables show that idle figure next to the peak instead of subtracting it.

An execution that fails is the result of its cell, recorded with the error text. An execution that
takes longer than 600 seconds is stopped and recorded as a timeout. Neither is retried or dropped.

The 600-second cap is checked by the engine between batches, so it does not fire in a process that is
blocked in swap. A cell whose process is still running 1,200 seconds after it started is therefore
killed by the harness and recorded as a timeout, with no statistics. This rule was added after the
first ablation attempt, when quarry at batch size 1 on Q5 grew to a 68 GB footprint and ran for over
an hour without reaching the cap. Results of the `main` suite (taken before the rule) are unaffected:
none of their cells came near either limit.

## Correctness before timing

Before the `main` suite times anything, quarry's answer to each of the four queries on the SF1
data is compared with DuckDB's, with the comparator of the differential tests
(`tests/differential/compare.py`: exact for integers, decimals and dates; relative error 1e-12 for
`AVG`). If any answer differs, the suite stops and writes nothing.

## A quiet machine

Other agents and services are stopped before a suite starts. Before every execution, including the
warm-up, the harness reads the 1-minute load average. While it is above 1.5 the harness waits, for
up to 2 minutes, then runs anyway and flags the cell. Every execution records the load average it
saw and how long it waited. The load the benchmark itself causes counts. The `smoke` suite, which
only checks that the harness works and publishes no numbers, records the load but does not wait.

## Suites

- `main`: Q1, Q3, Q5 and Q6 on `sf1`, for quarry, DuckDB `threads=1` and DuckDB default threads.
- `ablation`, quarry only unless stated, each row compared with the same query at the defaults:
  - batch size 1, 64, 1024, 2048 and 8192 on all four queries;
  - `--disable-rule prune_row_groups` on Q1 and Q6 over `sf1-sorted`, and over `sf1`; the
    default-rules baseline is measured on both exports, and DuckDB `threads=1` on `sf1-sorted`;
  - `--disable-rule prune_columns` on all four queries;
  - `--disable-rule order_joins` on Q3 and Q5.
- `spill`: Q3, Q5 and the high-cardinality aggregate
  `SELECT l_orderkey, SUM(l_extendedprice) FROM lineitem GROUP BY l_orderkey` (about 1.5 million
  groups at SF1) at unlimited, 256 MB, 64 MB and 16 MB (`--memory-limit`, powers of 1000), with the
  time and the bytes spilled. Q1 and Q6 have a handful of groups and never spill under any budget,
  so they are not in this table. The budget bounds operator state only. It does not bound the
  Parquet reader's row-group buffers, so the table also reports peak RSS. Under a small budget a
  sort, or a deep join, may be refused with `resource exhausted: ...`; a refused cell shows the
  refusal as its result and is not dropped or retried with a bigger budget.
- `smoke`: the `main` suite on the SF0.01 test data, for CI.

## What gets recorded

`uv run bench/run_bench.py --suite {main,ablation,spill,smoke} --data <dir>` writes
`bench/results/<suite>-<UTC timestamp>.json` with the repository commit, `tree_dirty`, the machine,
the compiler and flags, the Arrow version and build flags, the DuckDB version, the data
description, and for every (engine, configuration, query) the runs, median, minimum, maximum, peak
RSS and the load average before each run. Paths in a result file are relative to the repository
root. The script refuses (exit status 2) to start or to write when `git status --porcelain` lists
anything outside `bench/results/`, so `tree_dirty` in a written file is always `false`.

The tables in the README and in this directory's documents are rendered from the newest result of
each suite by `bench/render_tables.py`, between `<!-- bench:NAME -->` markers.
`scripts/check-readme-numbers.sh` re-renders them and fails when a number differs from the
committed results.

## Profile

`docs/where-time-goes.md` combines, for each query, the per-operator times from `EXPLAIN ANALYZE`
with macOS `sample` taken during a looping run, and classifies the top frames as Parquet decode,
filter, hash build, probe, aggregate, sort or other. It names the three largest gaps to DuckDB and
ties each to a frame or an operator; where it has no frame or number behind a claim, it says
nothing.

## Results

The tables below are rendered, not typed. They stay empty until a result is committed.

### Environment

<!-- bench:environment -->
Measured on Apple M2 (8 cores, 16 GB), macOS 26.6.2. quarry: Apple clang version 21.0.0 (clang-2100.3.34.2), Release build, flags `-O3 -DNDEBUG`, Arrow 26.0.1 (static). DuckDB 2.0.1. Data: TPC-H-derived, scale factor 1, Parquet row groups of 122,880 rows (snappy), 308 MiB.
<!-- /bench:environment -->

### quarry against DuckDB

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

### Memory

<!-- bench:memory -->
| Query | quarry peak RSS | quarry operator state | quarry scan buffers | DuckDB threads=1 peak RSS | DuckDB default peak RSS | Python + DuckDB idle RSS |
|---|---|---|---|---|---|---|
| Q1 | 38 | 0 | 19 | 99 | 206 | 85 |
| Q3 | 65 | 13 | 12 | 122 | 294 | 85 |
| Q5 | 6,288 | 2,612 | 12 | 128 | 285 | 86 |
| Q6 | 26 | 0 | 13 | 96 | 183 | 86 |

MiB. The DuckDB peaks include the Python interpreter, `duckdb` and `pyarrow`; the idle column is that process's RSS just before its first query and is not subtracted. quarry's operator state and scan buffers are the per-query Arrow pool peaks that `--explain-analyze` prints.

Source: `bench/results/main-20261121T065519Z.json`.
<!-- /bench:memory -->

### Ablation: batch size

<!-- bench:ablation-batch -->
| Query | batch 1 | batch 64 | batch 1024 | batch 2048 | batch 8192 |
|---|---|---|---|---|---|
| Q1 | 94,747 (92,067-95,112) | 2,051 (2,025-2,055) | 669.4 (669.2-690.9) | 619.4 (606.3-625.4) | 566.1 (565.5-566.5) |
| Q3 | 13,824 (13,643-14,105) | 1,119 (1,117-1,123) | 364.8 (359.7-368.5) | 319.3 (318.6-326.4) | 281.9 (281.3-282.9) |
| Q5 | timeout | 20,118 (19,414-20,400) | 5,328 (5,288-5,711) | 4,721 (4,707-4,841) | 4,251 (4,242-4,625) |
| Q6 | 46,636 (46,159-48,192) | 1,167 (1,164-1,236) | 371.6 (371.4-371.9) | 344.4 (342.3-348.4) | 321.3 (317.6-325.9) |

Source: `bench/results/ablation-20261121T093212Z.json` (commit `a9d26080932f`, 2026-11-21T09:32:12Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry batch_size=1 Q1, quarry batch_size=64 Q5.
<!-- /bench:ablation-batch -->

### Ablation: row-group pruning

<!-- bench:ablation-pruning -->
| lineitem | Query | quarry | quarry, prune_row_groups off | DuckDB threads=1 |
|---|---|---|---|---|
| sorted by l_shipdate | Q1 | 506.3 (505.9-506.9) | 497.5 (495.4-503.2) | 113.9 (113.8-115.8) |
| sorted by l_shipdate | Q6 | 58.9 (58.8-59.1) | 312.6 (311.8-354.9) | 11.3 (11.3-11.9) |
| dbgen order | Q1 | 605.4 (601.4-609.3) | 604.0 (601.7-639.2) | not run |
| dbgen order | Q6 | 342.3 (341.8-363.1) | 341.8 (341.5-345.1) | not run |

Source: `bench/results/ablation-20261121T093212Z.json` (commit `a9d26080932f`, 2026-11-21T09:32:12Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry batch_size=1 Q1, quarry batch_size=64 Q5.
<!-- /bench:ablation-pruning -->

### Ablation: optimizer rules

<!-- bench:ablation-rules -->
| Rule switched off | Query | quarry, all rules | quarry, rule off |
|---|---|---|---|
| `prune_columns` | Q1 | 618.4 (617.7-645.6) | 1,044 (1,040-1,046) |
| `prune_columns` | Q3 | 318.5 (318.3-320.2) | 1,258 (1,254-1,263) |
| `prune_columns` | Q5 | 4,777 (4,701-4,876) | timeout |
| `prune_columns` | Q6 | 344.4 (343.3-348.7) | 856.7 (852.3-891.3) |
| `order_joins` | Q3 | 318.5 (318.3-320.2) | 321.0 (317.8-329.1) |
| `order_joins` | Q5 | 4,777 (4,701-4,876) | 444.5 (444.0-449.0) |

Source: `bench/results/ablation-20261121T093212Z.json` (commit `a9d26080932f`, 2026-11-21T09:32:12Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry batch_size=1 Q1, quarry batch_size=64 Q5.
<!-- /bench:ablation-rules -->

### Cost of spilling

<!-- bench:spill -->
| Query | Memory limit | Time (ms) | Spilled (MiB) | Operator state peak (MiB) | Peak RSS (MiB) |
|---|---|---|---|---|---|
| Q3 | unlimited | 322.1 (318.6-345.7) | 0 | 13 | 67 |
| Q3 | 256MB | 335.1 (329.2-342.5) | 0 | 13 | 66 |
| Q3 | 64MB | 330.6 (329.8-340.1) | 0 | 13 | 66 |
| Q3 | 16MB | 1,602 (1,596-1,617) | 98 | 6 | 46 |
| Q5 | unlimited | 4,779 (4,693-4,881) | 0 | 2,612 | 6,288 |
| Q5 | 256MB | 32,253 (32,151-32,505) | 2,264 | 217 | 755 |
| Q5 | 64MB | 118,812 (118,690-119,128) | 3,965 | 54 | 297 |
| Q5 | 16MB | 118,568 (118,208-118,624) | 4,623 | 14 | 87 |
| High-cardinality aggregate | unlimited | 300.7 (300.0-302.4) | 0 | 0 | 265 |
| High-cardinality aggregate | 256MB | 321.6 (319.9-329.1) | 0 | 0 | 265 |
| High-cardinality aggregate | 64MB | 761.4 (754.8-773.5) | 36 | 0 | 165 |
| High-cardinality aggregate | 16MB | 727.5 (717.3-759.7) | 36 | 0 | 86 |

The limit bounds operator state only, not the Parquet reader's row-group buffers, so peak RSS stays above it. Batches read back from spill files use Arrow's default pool and are outside the operator-state peak (docs/design/0006-memory-and-spill.md). A refused cell is the result of that cell.

Source: `bench/results/spill-20261121T105922Z.json` (commit `a9d26080932f`, 2026-11-21T10:59:22Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry 256MB Q5, quarry 64MB Q5, quarry 16MB Q5.
<!-- /bench:spill -->
