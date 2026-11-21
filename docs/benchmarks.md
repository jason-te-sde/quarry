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
without mimalloc (the system allocator). <!-- const: the machine the suites were run on; every result file records it -->

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
quarry also reports the operator-pool peak and the scan-pool peak from its own memory pools
(`pool_peak_bytes` and `scan_pool_peak_bytes`, the same two numbers `--explain-analyze` prints).
The DuckDB figure includes the Python interpreter, the `duckdb` and `pyarrow` modules and the
connection, so each DuckDB worker also reports its RSS just before its first query, and the
tables show that idle figure next to the peak instead of subtracting it.

An execution that fails is the result of its cell, recorded with the error text. An execution that
takes longer than 600 seconds is stopped and recorded as a timeout. Neither is retried or dropped. <!-- const: the harness limit in bench/run_bench.py -->

The 600-second cap is checked by the engine between batches, so it does not fire in a process that is
blocked in swap. A cell whose process is still running 1,200 seconds after it started is therefore
killed by the harness and recorded as a timeout, with no statistics. <!-- const: the limits of bench/run_bench.py -->

This rule was added after the first ablation attempt, when quarry at batch size 1 on Q5 grew to a
68 GB footprint and ran for over an hour without reaching the cap. The footprint was read with
`footprint` while the process was running; that attempt was killed and left no result file, so no
committed result holds the figure. <!-- obs: read by hand with `footprint` during the first, killed ablation attempt -->
Results of the `main` suite (taken before the rule) are unaffected: none of their cells came near
either limit.

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
  so they are not in this table. The budget bounds operator state. It does not bound the
  Parquet reader's row-group buffers, so the table also reports peak RSS. Under a small budget a
  sort, or a deep join, may be refused with `resource exhausted: ...`; a refused cell shows the
  refusal as its result and is not dropped or retried with a bigger budget. <!-- const: the suite's definition in bench/run_bench.py -->
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

## Addendum: the suites are re-run after the fixes of the whole-branch review

Four changes land on the measured paths, so the three suites are re-run on the commit that carries
them:

- the scan asks the Parquet reader for leaf columns instead of Arrow field positions, and checks the
  cancel flag and the deadline once per row group and once per batch it reads;
- the hash tables' storage (key index, key arena, aggregate states, join chains) is allocated from the
  query's memory pool, and a table pays for a growth before it allocates it, so the allocator and the
  order of allocations on the join and aggregate paths are different;
- `AVG` and `COUNT` produce their output per batch of groups instead of recomputing every group for
  each batch (the high-cardinality aggregate and Q1 are the measured queries that use them);
- batches read back from spill files are allocated from the query's pool.

All three suites (`main`, `ablation`, `spill`) are re-run on that commit, one at a time, on a quiet
machine, and each table cites its own commit. The call graphs and operator times under
`bench/results/profile/` are taken again for every query, with the procedure above, because the
allocator change touches every query that joins or aggregates and the scan change every query. The
protocol is otherwise unchanged: how a run is timed, repeated and judged is as before. The earlier
results are kept under `bench/results/before-review-fixes/` (and their profiles under
`bench/results/profile/before-review-fixes/`) and are labelled "before the fixes of the whole-branch
review" wherever they are cited.

What the fixes did to the numbers, from the two `main` results (the ratio is quarry's median over
single-threaded DuckDB's in the same file):

<!-- bench:fix-wave -->
| Query | quarry before (ms) | quarry after (ms) | ratio before | ratio after | quarry time change |
|---|---|---|---|---|---|
| Q1 | 608.4 | 608.3 | 4.25 | 4.31 | -0.0% |
| Q3 | 323.3 | 321.3 | 3.59 | 3.57 | -0.6% |
| Q5 | 577.1 | 590.5 | 6.09 | 6.29 | +2.3% |
| Q6 | 344.3 | 342.4 | 6.10 | 6.05 | -0.5% |

Before: `bench/results/before-review-fixes/main-20261121T131055Z.json` (commit `479f7227df54`). After: `bench/results/main-20261121T163719Z.json` (commit `2676a9805ba8`). The ratio is quarry's median over DuckDB's, `threads=1`, in the same result file.
<!-- /bench:fix-wave -->

The four queries moved by a few percent, which is the run-to-run spread of these files. The times
rose where a limit is tight. Q3 at `--memory-limit 16MB` took <!-- n:fixes.before.spill.q3.16MB.ms -->1,610<!-- /n --> ms before and takes <!-- n:spill.q3.16MB.ms -->1,954<!-- /n --> ms now, and Q5 at that limit
took <!-- n:fixes.before.spill.q5.16MB.ms -->7,375<!-- /n --> ms and takes <!-- n:spill.q5.16MB.ms -->8,124<!-- /n --> ms. The high-cardinality aggregate took <!-- n:fixes.before.spill.agg.64MB.ms -->758.8<!-- /n --> ms at `64MB` and
<!-- n:fixes.before.spill.agg.16MB.ms -->725.6<!-- /n --> ms at `16MB` before, and takes <!-- n:spill.agg.64MB.ms -->929.5<!-- /n --> ms and <!-- n:spill.agg.16MB.ms -->927.4<!-- /n --> ms now. The bytes spilled did not change: Q3 at `16MB`
wrote <!-- n:fixes.before.spill.q3.16MB.spilled -->102.99<!-- /n --> MB before and writes <!-- n:spill.q3.16MB.spilled -->102.98<!-- /n --> MB now, Q5 at `16MB` <!-- n:fixes.before.spill.q5.16MB.spilled -->128.18<!-- /n --> MB both times, and the aggregate at `64MB`
<!-- n:fixes.before.spill.agg.64MB.spilled -->37.54<!-- /n --> MB both times. The cause of the slower times was not measured. The cells that do not spill did not
move for Q3 (<!-- n:fixes.before.spill.q3.unlimited.ms -->323.2<!-- /n --> ms before, <!-- n:spill.q3.unlimited.ms -->320.5<!-- /n --> ms now) or Q5 (<!-- n:fixes.before.spill.q5.unlimited.ms -->589.5<!-- /n --> ms, <!-- n:spill.q5.unlimited.ms -->587.8<!-- /n --> ms); the aggregate's moved from
<!-- n:fixes.before.spill.agg.unlimited.ms -->302.0<!-- /n --> ms to <!-- n:spill.agg.unlimited.ms -->318.2<!-- /n --> ms. The high-cardinality aggregate's operator pool peak without a limit went from
<!-- n:fixes.before.spill.agg.unlimited.state -->0.05<!-- /n --> MB to <!-- n:spill.agg.unlimited.state -->154.14<!-- /n --> MB, because its state is now allocated from the pool. The
`prune_columns` and `order_joins` rows of the ablation, and Q5 with `order_joins` off being faster than
with it on, are as before.

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

1. The operator times, for every `<name>` (`q1`, `q3`, `q5`, `q6`, and `q5-without-order_joins`, which is
   Q5 with `--disable-rule order_joins` added):
   `build/release/tools/quarry query --data bench/data/sf1 -f tests/tpch/queries/<query>.sql --explain-analyze`,
   saved as `bench/results/profile/explain-analyze-<name>.txt`, where `<query>` is `<name>` up to the
   first `-`.
2. The samples, from the quarry process itself (the DuckDB worker is never sampled): start
   `build/release/tools/quarry query --data bench/data/sf1 -c "$(cat tests/tpch/queries/<name>.sql)" --repeat N --timing-json /dev/null`
   in the background, with N large enough that the loop is still running 10 seconds later (a query
   of 0.3 s needs N of at least 40, one of 4 s needs 5; the loop repeats the same query, so N does
   not change what is sampled). Wait until the warm-up has finished, then run
   `sample <pid> 10 1 -file callgraph-<name>.txt` (10 seconds, one sample per millisecond, all
   threads). Stop the quarry process afterwards. <!-- const: the procedure's parameters -->
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
| Q1 | 608.3 (603.3-611.5) | 141.0 (140.7-179.7) | 33.3 (33.2-34.4) | 4.31 |
| Q3 | 321.3 (318.8-321.4) | 90.0 (89.6-90.1) | 27.9 (27.7-29.4) | 3.57 |
| Q5 | 590.5 (575.0-610.3) | 93.9 (93.4-94.9) | 29.7 (29.5-30.1) | 6.29 |
| Q6 | 342.4 (342.0-356.3) | 56.6 (55.7-58.6) | 15.8 (15.5-16.0) | 6.05 |

A ratio above 1.00 means quarry is slower than single-threaded DuckDB.

Source: `bench/results/main-20261121T163719Z.json` (commit `2676a9805ba8`, 2026-11-21T16:37:19Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:main -->

### Memory

<!-- bench:memory -->
| Query | quarry peak RSS | quarry operator pool peak | quarry scan pool peak | DuckDB threads=1 peak RSS | DuckDB default peak RSS | Python + DuckDB idle RSS |
|---|---|---|---|---|---|---|
| Q1 | 53 | 0 | 19 | 104 | 214 | 86 |
| Q3 | 63 | 26 | 12 | 116 | 302 | 86 |
| Q5 | 125 | 56 | 12 | 124 | 279 | 85 |
| Q6 | 26 | 0 | 13 | 96 | 179 | 86 |

MiB. The DuckDB peaks include the Python interpreter, `duckdb` and `pyarrow`; the idle column is that process's RSS just before its first query and is not subtracted. quarry's two pool peaks are the per-query Arrow pool peaks that `--explain-analyze` prints. The operator pool holds the hash tables' storage (key index, key arena, aggregate states, join chains), sort and build-row buffers, spill buffers and expression temporaries; the scan pool holds the Parquet reader's row-group buffers. Neither includes the result table.

Source: `bench/results/main-20261121T163719Z.json`.
<!-- /bench:memory -->

### Ablation: batch size

<!-- bench:ablation-batch -->
| Query | batch 1 | batch 64 | batch 1024 | batch 2048 | batch 8192 |
|---|---|---|---|---|---|
| Q1 | 95,075 (94,053-96,064) | 2,052 (2,031-2,058) | 669.0 (668.5-670.1) | 618.5 (615.7-618.9) | 562.7 (552.1-565.8) |
| Q3 | 13,984 (13,594-14,148) | 1,120 (1,110-1,142) | 361.6 (360.6-363.7) | 321.0 (318.7-323.5) | 281.6 (281.0-284.3) |
| Q5 | 22,524 (22,374-22,604) | 4,072 (4,041-4,094) | 733.4 (715.6-750.7) | 574.2 (572.5-609.2) | 468.5 (467.3-470.6) |
| Q6 | 47,436 (44,828-48,205) | 1,151 (1,150-1,153) | 376.8 (372.6-378.1) | 342.4 (341.9-343.6) | 319.7 (319.6-321.3) |

Source: `bench/results/ablation-20261121T164037Z.json` (commit `2676a9805ba8`, 2026-11-21T16:40:37Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry batch_size=1 Q1 (default data), quarry batch_size=1 Q3 (default data), quarry batch_size=1 Q6 (default data), quarry batch_size=64 Q1 (default data).
<!-- /bench:ablation-batch -->

### Ablation: row-group pruning

<!-- bench:ablation-pruning -->
| lineitem | Query | quarry | quarry, prune_row_groups off | DuckDB threads=1 |
|---|---|---|---|---|
| sorted by l_shipdate | Q1 | 503.0 (494.9-509.4) | 506.9 (506.5-507.4) | 114.4 (114.1-116.0) |
| sorted by l_shipdate | Q6 | 58.8 (58.7-59.1) | 313.2 (311.3-316.1) | 11.3 (11.0-11.6) |
| dbgen order | Q1 | 617.4 (616.8-618.3) | 618.3 (616.8-632.6) | not run |
| dbgen order | Q6 | 348.3 (341.9-350.0) | 348.1 (343.8-350.1) | not run |

Source: `bench/results/ablation-20261121T164037Z.json` (commit `2676a9805ba8`, 2026-11-21T16:40:37Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:ablation-pruning -->

### Ablation: optimizer rules

<!-- bench:ablation-rules -->
| Rule switched off | Query | quarry, all rules | quarry, rule off |
|---|---|---|---|
| `prune_columns` | Q1 | 619.7 (616.7-635.1) | 1,042 (1,041-1,052) |
| `prune_columns` | Q3 | 318.2 (318.0-357.2) | 1,266 (1,254-1,268) |
| `prune_columns` | Q5 | 577.4 (575.0-595.1) | 1,705 (1,689-1,725) |
| `prune_columns` | Q6 | 347.8 (346.2-349.7) | 868.4 (864.5-882.4) |
| `order_joins` | Q3 | 318.2 (318.0-357.2) | 319.8 (318.3-320.5) |
| `order_joins` | Q5 | 577.4 (575.0-595.1) | 448.8 (444.7-450.5) |

Source: `bench/results/ablation-20261121T164037Z.json` (commit `2676a9805ba8`, 2026-11-21T16:40:37Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
The 1-minute load average stayed at or below 1.5 before every run of every measured cell.
<!-- /bench:ablation-rules -->

### Cost of spilling

<!-- bench:spill -->
| Query | Memory limit (MB) | Time (ms) | Spilled (MB) | Operator pool peak (MB) | Peak RSS (MB) |
|---|---|---|---|---|---|
| Q3 | unlimited | 320.5 (319.3-323.4) | 0.00 | 27.20 | 81.95 |
| Q3 | 256MB | 332.7 (329.6-357.8) | 0.00 | 29.12 | 65.91 |
| Q3 | 64MB | 334.5 (329.8-370.7) | 0.00 | 29.12 | 75.58 |
| Q3 | 16MB | 1,954 (1,945-2,038) | 102.98 | 9.68 | 46.92 |
| Q5 | unlimited | 587.8 (582.5-620.4) | 0.00 | 58.76 | 120.01 |
| Q5 | 256MB | 588.8 (586.6-598.8) | 0.00 | 57.05 | 120.82 |
| Q5 | 64MB | 587.5 (584.1-621.8) | 0.00 | 57.05 | 120.90 |
| Q5 | 16MB | 8,124 (7,980-8,213) | 128.18 | 17.09 | 107.99 |
| High-cardinality aggregate | unlimited | 318.2 (315.9-323.8) | 0.00 | 154.14 | 272.94 |
| High-cardinality aggregate | 256MB | 373.0 (340.0-386.3) | 0.00 | 154.14 | 276.61 |
| High-cardinality aggregate | 64MB | 929.5 (923.0-965.9) | 37.54 | 43.03 | 124.62 |
| High-cardinality aggregate | 16MB | 927.4 (882.2-940.5) | 37.55 | 10.79 | 74.50 |

The limit bounds operator state, not the Parquet reader's row-group buffers, so peak RSS stays above it. The operator pool peak is what the query's operators allocated through Arrow's memory pool: the hash tables' storage (key index, key arena, aggregate states, join chains), sort and build-row buffers, spill buffers, batches read back from spill files and expression temporaries. It excludes the Parquet reader's row-group buffers (their own pool), the result table, and the strings that MIN and MAX of a VARCHAR hold (docs/design/0006-memory-and-spill.md). The limit and the three sizes are in the same unit, MB of 10^6 bytes, as `--memory-limit` counts them. A join's build rows are charged once, but a join makes a second copy of them when it concatenates its build side, and that copy can sit in the operator pool, so the pool peak can pass the limit: Q5 at 16MB is above it (known issue in the CHANGELOG). A refused cell is the result of that cell.

Source: `bench/results/spill-20261121T180222Z.json` (commit `2676a9805ba8`, 2026-11-21T18:02:22Z); times are milliseconds, median (min-max) of 5 runs after 1 warm-up.
Cells that ran with a 1-minute load average above 1.5 after the wait: quarry 256MB High-cardinality aggregate (default data), quarry 16MB High-cardinality aggregate (default data).
<!-- /bench:spill -->
