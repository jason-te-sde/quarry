# Where the time goes

What the profile of quarry on the SF1 data says about the four queries, and where quarry is
slower than single-threaded DuckDB. The protocol is in [benchmarks.md](benchmarks.md); the numbers
here are from the files in `bench/results/`, rendered by `bench/render_tables.py`.

Method: `quarry query --explain-analyze` gives each operator's own time; `sample <pid> 10` on macOS
ran for 10 seconds while `quarry query --repeat N` looped one query, and
`bench/classify_samples.py` attributes every sample to the nearest operator frame on its call stack
(a pull-based operator's frame contains its children's, so the nearest one did the work). The
samples come from a profiling run, not from a measured run: they say where the time is, not how
much of it there is. Scan time includes the filter pushed into the scan, so the scan's own time in
`EXPLAIN ANALYZE` is decode plus filter.

## The gaps to DuckDB

<!-- bench:gaps -->
| Query | quarry (ms) | DuckDB threads=1 (ms) | Gap (ms) | Ratio |
|---|---|---|---|---|
| Q5 | 4,730 | 93.8 | 4,636 | 50.40 |
| Q1 | 616.6 | 141.5 | 475.0 | 4.36 |
| Q6 | 341.8 | 56.8 | 285.0 | 6.02 |
| Q3 | 319.5 | 90.5 | 229.0 | 3.53 |

Medians from `bench/results/main-20261121T065519Z.json`, largest gap first.
<!-- /bench:gaps -->

Every figure in the three sections below is copied from the tables on this page and in
[benchmarks.md](benchmarks.md). DuckDB itself was not profiled, so each gap is tied to what quarry
spends the time on, not to what DuckDB does differently.

### 1. Q5: the optimizer picks a join order that builds 18 million rows

Q5 is the largest gap by far, and it is the plan, not the engine. With every rule on, the plan joins
`customer` to the 2,003 ASIA suppliers on `c_nationkey = s_nationkey` first (`HashJoin #7`), which
produces 12,093,991 rows where the plan estimated 15,000. It then joins `orders` to that result
(`HashJoin #5`: 18,435,748 rows, estimated 13,500) and finally builds a hash table of those 18
million rows to probe with `lineitem` (`HashJoin #3`, 3,545 MiB of operator state, 73% of the
operator time). 72% of the samples are in hash build, most of them in `quarry::KeyIndex::Find`
(27%) and `quarry::KeyIndex::Insert` (16%) called from `JoinHashTable::Insert`, and the process
peaks at 6.3 GiB (the memory table in [benchmarks.md](benchmarks.md)).

The estimate is wrong because of how `EstimateJoinRows` in `src/planner/cardinality.cc` divides: it
divides the product of the two input sizes by the row count of the first join key's base table, as if
that key were unique. `s_nationkey` has 25 distinct values among 10,000 suppliers, not 10,000, and
the 15,000 equals 150,000 customers times the 1,000 estimated suppliers, over those 10,000 supplier rows. With `--disable-rule order_joins` the plan joins `orders` to `customer` first and probes the result with `lineitem` (`explain-q5-without-order_joins.txt` against `explain-q5.txt`), the largest operator state is 22
MiB, and the query runs in the time the ablation table shows: about a tenth of the default. That is
the cause of the gap; the rest of Q5 is the same engine as Q3. It is also why the Q5 spill rows are
so slow: at 256 MB, 64 MB and 16 MB the same plan runs and spills several GiB (spill table in [benchmarks.md](benchmarks.md)).

The batch-size ablation has one more observation about the same plan: at batch size 1 the Q5 cell
was killed by the harness at its cell limit, after a footprint of 68 GB had been reached.

### 2. Q1: aggregation over 5.9 million rows costs more than DuckDB's whole query

`HashAggregate #2` has 392 ms of own time, 64% of the operator time and 64% of the samples, more
than the 141 ms DuckDB needs for the entire query. The group count is four, so the time is per-row
work: `_platform_memmove` (11%) and `quarry::Gather` (10%) copy rows, `SumState::Update` takes 8%, `eval_internal::RunChecked` (checked decimal
arithmetic of the aggregate arguments) 4%, and `HashBytes` 3% hashes the encoded group key of every
row. The scan under it (222 ms) is 22% Parquet decode and is dominated by `snappy::DecompressBranchless`
and the string builder (`arrow::BaseBinaryBuilder::Append`) that materialises `l_returnflag` and
`l_linestatus` as strings for 5.9 million rows.

### 3. Q6: the filter is evaluated for every row of every conjunct

`Scan #2` is 98% of the operator time and 78% of the samples are the filter pushed into it:
`eval_internal::EvalLogical` (27%), `CompareFixed` (20%), the copies around them (`_platform_memmove`
14%, `memcpy` 6%) and `RepeatedArrayFactory::CreateBufferOf` (4%), Arrow's array-from-scalar helper, which the comparisons call for their literal operands. The query keeps 114,160 of 6,001,215 rows, so almost all the work is
evaluating four comparisons and three `AND`s over rows that are then dropped. Row-group pruning
cannot help on the default file, because the dates are spread across every row group; on the
`l_shipdate`-sorted file it can, and the pruning table shows both effects.

### Q3

Q3's gap is the smallest. 24% of the samples are Parquet decode (17% in snappy decompression, which
DuckDB also pays), 36% are the probe (`KeyIndex::Find` 12%, `HashBytes` 5%, `Gather` 5%) and 21%
are the filters pushed into the scans. No single frame stands out the way Q5's does.

## Operators

<!-- bench:profile-operators -->
| Query | Operator | Own time (ms) | Share of operator time | Rows out | Peak state |
|---|---|---|---|---|---|
| Q1 | HashAggregate #2 | 391.9 | 64% | 4 | 960B |
| Q1 | Scan #3 | 221.7 | 36% | 5,916,591 | 0B |
| Q1 | Sort #0 | 0.0 | 0% | 4 | 440B |
| Q3 | Scan #4 | 121.2 | 38% | 3,241,776 | 0B |
| Q3 | HashJoin #3 | 103.5 | 32% | 30,519 | 21.4MiB |
| Q3 | Scan #6 | 38.3 | 12% | 727,305 | 0B |
| Q5 | HashJoin #3 | 3,871 | 73% | 7,243 | 3545.0MiB |
| Q5 | HashJoin #5 | 986.9 | 19% | 18,435,748 | 691.2MiB |
| Q5 | HashJoin #7 | 262.4 | 5% | 12,093,991 | 91.0KiB |
| Q5, `order_joins` off | HashJoin #6 | 198.0 | 44% | 910,519 | 22.2MiB |
| Q5, `order_joins` off | Scan #7 | 80.4 | 18% | 6,001,215 | 0B |
| Q5, `order_joins` off | Scan #10 | 49.9 | 11% | 227,597 | 0B |
| Q6 | Scan #2 | 345.7 | 98% | 114,160 | 0B |
| Q6 | HashAggregate #1 | 8.5 | 2% | 1 | 281B |
| Q6 | Project #0 | 0.0 | 0% | 1 | 0B |

The three operators with the most own time in each `--explain-analyze` run on SF1 (`bench/results/profile/explain-analyze-*.txt`).
<!-- /bench:profile-operators -->

## Samples by category

<!-- bench:profile-categories -->
| Query | Parquet decode | filter | scan other | hash build | probe | aggregate | sort | other | samples |
|---|---|---|---|---|---|---|---|---|---|
| Q1 | 22% | 9% | 3% | 0% | 0% | 64% | 0% | 2% | 7,619 |
| Q3 | 24% | 21% | 3% | 7% | 36% | 6% | 0% | 2% | 7,625 |
| Q5 | 2% | 1% | 0% | 72% | 25% | 0% | 0% | 0% | 7,578 |
| Q5, `order_joins` off | 18% | 7% | 2% | 7% | 60% | 4% | 0% | 2% | 7,651 |
| Q6 | 15% | 78% | 4% | 0% | 0% | 3% | 0% | 1% | 7,592 |

Share of the samples taken on non-idle threads (`sample <pid> 10`, 1 ms interval) while `quarry query --repeat` looped the query on SF1; `bench/classify_samples.py` explains the categories. Source: `bench/results/profile/classified.json`.
<!-- /bench:profile-categories -->

Functions on top of the stack:

<!-- bench:profile-frames -->
| Query | Function on top of the stack | Category | Share of samples |
|---|---|---|---|
| Q1 | `_platform_memmove` | aggregate | 10.6% |
| Q1 | `quarry::Gather` | aggregate | 10.4% |
| Q1 | `quarry::SumState::Update` | aggregate | 7.6% |
| Q1 | `arrow::BaseBinaryBuilder::Append` | Parquet decode | 4.8% |
| Q1 | `snappy::DecompressBranchless` | Parquet decode | 4.5% |
| Q3 | `snappy::DecompressBranchless` | Parquet decode | 17.4% |
| Q3 | `quarry::KeyIndex::Find` | probe | 12.1% |
| Q3 | `quarry::EvaluatePredicate` | filter | 6.2% |
| Q3 | `_platform_memmove` | filter | 4.8% |
| Q3 | `quarry::HashBytes` | probe | 4.7% |
| Q5 | `quarry::KeyIndex::Find` | hash build | 27.2% |
| Q5 | `quarry::KeyIndex::Insert` | hash build | 16.2% |
| Q5 | `quarry::Gather` | probe | 9.2% |
| Q5 | `_platform_memmove` | hash build | 7.4% |
| Q5 | `quarry::KeyIndex::Find` | probe | 4.2% |
| Q5, `order_joins` off | `quarry::KeyIndex::Find` | probe | 20.2% |
| Q5, `order_joins` off | `snappy::DecompressBranchless` | Parquet decode | 12.6% |
| Q5, `order_joins` off | `quarry::HashBytes` | probe | 7.6% |
| Q5, `order_joins` off | `_platform_memmove` | probe | 4.7% |
| Q5, `order_joins` off | `quarry::Gather` | probe | 4.7% |
| Q6 | `quarry::eval_internal::EvalLogical` | filter | 26.8% |
| Q6 | `quarry::eval_internal::CompareFixed` | filter | 19.7% |
| Q6 | `_platform_memmove` | filter | 14.2% |
| Q6 | `snappy::DecompressBranchless` | Parquet decode | 8.2% |
| Q6 | `DYLD-STUB$$memcpy` | filter | 5.6% |
<!-- /bench:profile-frames -->
