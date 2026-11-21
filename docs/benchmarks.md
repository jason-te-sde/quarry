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
_No result is committed for this table yet._
<!-- /bench:environment -->

### quarry against DuckDB

<!-- bench:main -->
_No result is committed for this table yet._
<!-- /bench:main -->

### Memory

<!-- bench:memory -->
_No result is committed for this table yet._
<!-- /bench:memory -->

### Ablation: batch size

<!-- bench:ablation-batch -->
_No result is committed for this table yet._
<!-- /bench:ablation-batch -->

### Ablation: row-group pruning

<!-- bench:ablation-pruning -->
_No result is committed for this table yet._
<!-- /bench:ablation-pruning -->

### Ablation: optimizer rules

<!-- bench:ablation-rules -->
_No result is committed for this table yet._
<!-- /bench:ablation-rules -->

### Cost of spilling

<!-- bench:spill -->
_No result is committed for this table yet._
<!-- /bench:spill -->
