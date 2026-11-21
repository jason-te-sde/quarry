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

## Addendum: the suites are re-run after the join-estimate fix

The first SF1 results showed Q5 at 50 times DuckDB's time. The cause was a known estimator bug: the
join estimate treated two foreign keys to `nation` as a unique key, so the greedy order built an
18-million-row hash table (`docs/where-time-goes.md`, ADR 0005). The fix caps a key pair's divisor at
its value domain from the Parquet min/max. All three suites (`main`, `ablation`, `spill`) are re-run
on the fixed commit, one at a time, and each table cites its own commit. The protocol is otherwise
unchanged, with these differences in what a result records: before timing, every variant of quarry
the suite runs has its answer checked against DuckDB's (not only the `main` configuration), and a
figure that was never measured (the warm-up and load flag of a cell that timed out or crashed) is
`null`, not zero or false. The earlier results are kept, under `bench/results/before-join-estimate-fix/`,
and are labelled "before the join-estimate fix" wherever they are cited. Nothing about how a run
is timed, repeated or judged has changed.

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

### Procedure

The call graphs under `bench/results/profile/` are taken as follows, on the SF1 default export,
with nothing else running.

1. The operator times: `build/release/tools/quarry query --data bench/data/sf1 -c "$(cat tests/tpch/queries/q5.sql)" --explain-analyze`,
   saved as `bench/results/profile/explain-analyze-<name>.txt` (`--disable-rule order_joins` for the
   `q5-without-order_joins` name).
2. The samples, from the quarry process itself (the DuckDB worker is never sampled): start
   `build/release/tools/quarry query --data bench/data/sf1 -c "$(cat tests/tpch/queries/<name>.sql)" --repeat N --timing-json /dev/null`
   in the background, with N large enough that the loop is still running 10 seconds later (a query
   of 0.3 s needs N of at least 40, one of 4 s needs 5; the loop repeats the same query, so N does
   not change what is sampled). Wait until the warm-up has finished, then run
   `sample <pid> 10 1 -file callgraph-<name>.txt` (10 seconds, one sample per millisecond, all
   threads). Stop the quarry process afterwards.
3. Keep the "Call graph:" section of the output, compress it as
   `bench/results/profile/callgraph-<name>.txt.gz`, and run `uv run bench/classify_samples.py`,
   which writes `classified.json`. Rendering the profile tables is `bench/render_tables.py`.

The samples describe where the time is in a profiling run. They are not a measured run and are
not compared with DuckDB.

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
| Q1 | 608.4 (603.3-609.8) | 143.0 (140.5-144.3) | 33.4 (32.5-33.6) | 4.25 |
| Q3 | 323.3 (321.6-325.0) | 90.0 (89.7-90.6) | 28.5 (27.5-28.7) | 3.59 |
| Q5 | 577.1 (575.3-587.4) | 94.7 (94.6-95.7) | 29.4 (28.3-29.8) | 6.09 |
| Q6 | 344.3 (342.6-348.7) | 56.4 (55.5-57.7) | 15.7 (15.4-15.9) | 6.10 |

A ratio above 1.00 means quarry is slower than single-threaded DuckDB.

Source: `bench/results/main-20261121T131055Z.json` (commit `479f7227df54`, 2026-11-21T13:10:55Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:main -->

### Memory

<!-- bench:memory -->
| Query | quarry peak RSS | quarry operator state | quarry scan buffers | DuckDB threads=1 peak RSS | DuckDB default peak RSS | Python + DuckDB idle RSS |
|---|---|---|---|---|---|---|
| Q1 | 38 | 0 | 19 | 99 | 212 | 85 |
| Q3 | 65 | 13 | 12 | 118 | 302 | 86 |
| Q5 | 114 | 34 | 12 | 127 | 279 | 85 |
| Q6 | 26 | 0 | 13 | 96 | 180 | 85 |

MiB. The DuckDB peaks include the Python interpreter, `duckdb` and `pyarrow`; the idle column is that process's RSS just before its first query and is not subtracted. quarry's operator state and scan buffers are the per-query Arrow pool peaks that `--explain-analyze` prints.

Source: `bench/results/main-20261121T131055Z.json`.
<!-- /bench:memory -->

### Ablation: batch size

<!-- bench:ablation-batch -->
| Query | batch 1 | batch 64 | batch 1024 | batch 2048 | batch 8192 |
|---|---|---|---|---|---|
| Q1 | 94,891 (94,363-95,103) | 2,068 (2,059-2,070) | 671.6 (671.0-672.3) | 621.2 (615.0-623.2) | 568.5 (567.9-569.7) |
| Q3 | 14,232 (14,088-14,375) | 1,125 (1,124-1,131) | 371.1 (369.7-381.8) | 326.9 (325.9-327.3) | 288.5 (287.5-293.6) |
| Q5 | 22,247 (22,191-22,347) | 4,055 (4,050-4,156) | 733.4 (731.8-742.8) | 588.6 (586.6-597.3) | 477.9 (476.9-481.3) |
| Q6 | 49,162 (48,881-49,301) | 1,157 (1,156-1,158) | 381.3 (381.1-381.5) | 352.8 (351.6-353.8) | 329.0 (328.8-329.6) |

Source: `bench/results/ablation-20261121T131139Z.json` (commit `479f7227df54`, 2026-11-21T13:11:39Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry batch_size=1 Q1 (default data).
<!-- /bench:ablation-batch -->

### Ablation: row-group pruning

<!-- bench:ablation-pruning -->
| lineitem | Query | quarry | quarry, prune_row_groups off | DuckDB threads=1 |
|---|---|---|---|---|
| sorted by l_shipdate | Q1 | 506.6 (506.2-545.9) | 507.5 (507.0-508.3) | 116.9 (116.6-117.7) |
| sorted by l_shipdate | Q6 | 60.3 (60.2-60.7) | 321.5 (320.9-322.2) | 11.3 (10.9-11.6) |
| dbgen order | Q1 | 619.2 (618.4-634.5) | 621.0 (618.4-626.4) | not run |
| dbgen order | Q6 | 351.5 (350.0-354.2) | 352.8 (351.7-353.2) | not run |

Source: `bench/results/ablation-20261121T131139Z.json` (commit `479f7227df54`, 2026-11-21T13:11:39Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:ablation-pruning -->

### Ablation: optimizer rules

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

### Cost of spilling

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
