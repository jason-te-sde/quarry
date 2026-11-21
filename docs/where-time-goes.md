# Where the time goes

What the profile of quarry on the SF1 data says about the four queries, and where quarry is
slower than single-threaded DuckDB. The protocol is in [benchmarks.md](benchmarks.md); the numbers
here are from the files in `bench/results/`, rendered by `bench/render_tables.py`.

Method: `quarry query --explain-analyze` gives each operator's own time; `sample <pid> 10` on macOS
ran for 10 seconds <!-- const: the procedure's parameters --> while `quarry query --repeat N` looped one query, and
`bench/classify_samples.py` attributes every sample to the nearest operator frame on its call stack
(a pull-based operator's frame contains its children's, so the nearest one did the work). The
samples come from a profiling run, not from a measured run: they say where the time is, not how
much of it there is. Scan time includes the filter pushed into the scan, so the scan's own time in
`EXPLAIN ANALYZE` is decode plus filter. The procedure is in [benchmarks.md](benchmarks.md).

Which profile belongs to which commit. The Q5 profile (`explain-analyze-q5.txt`, `callgraph-q5.txt.gz`,
`explain-q5.txt`) was taken at commit 479f722, the commit the new results come from, and describes the
plan that ships. The Q1, Q3, Q6 and Q5-with-`order_joins`-off profiles were taken at commit 546d5a5,
before the join-estimate fix; they stand because the fix does not change their plans (Q3's golden plan
is unchanged, Q1 and Q6 have no join, and `order_joins` off uses the FROM order whatever the
estimate says), and the only executor change since is how a join, sort or top-N concatenates its
input (07c0dc3). The Q5 profile of 546d5a5 is kept under `bench/results/profile/before-join-estimate-fix/`
and shown in the tables labelled "before the join-estimate fix". The results of that commit's
measurement are under `bench/results/before-join-estimate-fix/`.

## The gaps to DuckDB

<!-- bench:gaps -->
| Query | quarry (ms) | DuckDB threads=1 (ms) | Gap (ms) | Ratio |
|---|---|---|---|---|
| Q5 | 577.1 | 94.7 | 482.4 | 6.09 |
| Q1 | 608.4 | 143.0 | 465.4 | 4.25 |
| Q6 | 344.3 | 56.4 | 287.8 | 6.10 |
| Q3 | 323.3 | 90.0 | 233.3 | 3.59 |

Medians from `bench/results/main-20261121T131055Z.json`, largest gap first.
<!-- /bench:gaps -->

Every figure in the sections below is copied from the tables on this page and in
[benchmarks.md](benchmarks.md), and each is regenerated from the committed results by
`bench/render_tables.py`, so `scripts/check-readme-numbers.sh` fails when one is edited by hand.
DuckDB itself was not profiled, so each gap is tied to what quarry spends the time on, not to what
DuckDB does differently.

### 1. Q5: three joins over a lineitem probe, and an order that is still not the best one

Q5 is still the largest gap, ratio <!-- n:main.q5.ratio -->6.09<!-- /n --> to DuckDB with one thread, and it is
no longer the plan that is broken. The plan that ships joins `lineitem` to the ASIA suppliers first
(`HashJoin #6`: <!-- n:ea.q5.6.rows -->1,201,113<!-- /n --> rows out, <!-- n:ea.q5.6.time -->226.7<!-- /n --> ms of own time, <!-- n:ea.q5.6.share -->39<!-- /n -->% of the operator time), then joins
`orders` (`HashJoin #5`, <!-- n:ea.q5.5.rows -->182,183<!-- /n --> rows) and `customer` (`HashJoin #3`, <!-- n:ea.q5.3.time -->129.3<!-- /n --> ms, peak state
<!-- n:ea.q5.3.peak -->32.9MiB<!-- /n -->). The whole query peaks at <!-- n:main.q5.rss -->114<!-- /n --> MiB (<!-- n:main.q5.state -->34<!-- /n --> MiB of operator state). The samples are
<!-- n:cls.q5.probe -->54<!-- /n -->% probe and <!-- n:cls.q5.hash build -->23<!-- /n -->% hash build, with `KeyIndex::Find` called from the probe at
<!-- n:frame.q5.KeyIndex::Find.probe -->28.1<!-- /n -->% and Parquet decode at <!-- n:cls.q5.Parquet decode -->13<!-- /n -->% (<!-- n:frame.q5.snappy::DecompressBranchless.Parquet decode -->9.6<!-- /n -->% in snappy). That is the same
engine as in Q3: a hash probe per lineitem row plus the scan.

The order is still not the best one. With `--disable-rule order_joins` the plan uses the FROM order
and Q5 takes <!-- n:rules.order_joins.off.q5 -->447.0<!-- /n --> ms against <!-- n:rules.on.q5 -->573.2<!-- /n --> ms with the rule on (ablation table in
[benchmarks.md](benchmarks.md)). The plan estimates <!-- n:ea.q5.8.est -->1,000<!-- /n --> rows for the supplier join where it produces
<!-- n:ea.q5.8.rows -->2,003<!-- /n -->, and <!-- n:ea.q5.6.est -->600,122<!-- /n --> for `HashJoin #6` where it produces <!-- n:ea.q5.6.rows -->1,201,113<!-- /n -->. The likely reason is the
fixed selectivity of `r_name = 'ASIA'` (a tenth, where one of five regions matches); the greedy
order then prefers a plan that is not the cheapest. I did not test that reason. ADR 0005 lists
the estimator's remaining failure cases.

What was fixed. At commit 546d5a5 (the results under `bench/results/before-join-estimate-fix/`) Q5
took <!-- n:before.main.q5.quarry -->4,730<!-- /n --> ms, <!-- n:before.main.q5.ratio -->50.40<!-- /n -->x DuckDB's time, and peaked at <!-- n:before.main.q5.rss -->6,288<!-- /n --> MiB. The estimator
divided by the smaller base table's row count for every key pair, as if each key were unique, so
for `c_nationkey = s_nationkey`, two foreign keys to `nation`, it estimated <!-- n:ea.q5before.7.est -->15,000<!-- /n --> rows
and the join produced <!-- n:ea.q5before.7.rows -->12,093,991<!-- /n -->. The greedy order then built a hash table of
<!-- n:ea.q5before.5.rows -->18,435,748<!-- /n --> rows (`HashJoin #3`: <!-- n:ea.q5before.3.peak -->3545.0MiB<!-- /n --> of operator state, <!-- n:cls.q5before.hash build -->72<!-- /n -->% of the samples in hash
build). With `--disable-rule order_joins` that plan took <!-- n:before.rules.order_joins.off.q5 -->444.5<!-- /n --> ms. The fix caps a key
pair's divisor at the value domain read from the Parquet min/max (ADR 0005); the three suites were
re-run after it.

Two cells that did not finish before now do, both at the fixed commit: Q5 at batch size 1 takes
<!-- n:batch.1.q5 -->22,247<!-- /n --> ms and Q5 with `prune_columns` off takes <!-- n:rules.prune_columns.off.q5 -->1,704<!-- /n --> ms. The Q5 spill rows are no longer
slow in the same way: <!-- const: the spill suite's limits --> at 64 MB Q5 takes <!-- n:spill.q5.64MB.ms -->596.0<!-- /n --> ms and does not spill; at 16 MB it takes
<!-- n:spill.q5.16MB.ms -->7,375<!-- /n --> ms and spills <!-- n:spill.q5.16MB.spilled -->122<!-- /n --> MiB (spill table in [benchmarks.md](benchmarks.md)).

### 2. Q1: aggregating every lineitem row costs more than DuckDB's whole query

`HashAggregate #2` has <!-- n:ea.q1.2.time -->391.9<!-- /n --> ms of own time, <!-- n:ea.q1.2.share -->64<!-- /n -->% of the operator time and <!-- n:cls.q1.aggregate -->64<!-- /n -->% of the samples, more
than the <!-- n:main.q1.duckdb -->143.0<!-- /n --> ms DuckDB needs for the entire query. The group count is <!-- n:ea.q1.2.rows -->4<!-- /n -->, so the time is
per-row work: `_platform_memmove` (<!-- n:frame.q1._platform_memmove.aggregate -->10.6<!-- /n -->%) and `Gather` (<!-- n:frame.q1.Gather.aggregate -->10.4<!-- /n -->%) copy rows,
`SumState::Update` takes <!-- n:frame.q1.SumState::Update.aggregate -->7.6<!-- /n -->%, `RunChecked` (checked decimal arithmetic of the aggregate
arguments) <!-- n:frame.q1.RunChecked.aggregate -->4.4<!-- /n -->%, and `HashBytes` <!-- n:frame.q1.HashBytes.aggregate -->3.3<!-- /n -->% hashes the encoded group key of every row. The scan
under it (<!-- n:ea.q1.3.time -->221.7<!-- /n --> ms, <!-- n:ea.q1.3.rows -->5,916,591<!-- /n --> rows out) is <!-- n:cls.q1.Parquet decode -->22<!-- /n -->% Parquet decode, dominated by
`snappy::DecompressBranchless` (<!-- n:frame.q1.snappy::DecompressBranchless.Parquet decode -->4.5<!-- /n -->%) and the string builder
(`arrow::BaseBinaryBuilder::Append`, <!-- n:frame.q1.arrow::BaseBinaryBuilder::Append.Parquet decode -->4.8<!-- /n -->%) that materialises `l_returnflag` and `l_linestatus` as strings
for every row.

### 3. Q6: the filter is evaluated for every row of every conjunct

`Scan #2` is <!-- n:ea.q6.2.share -->98<!-- /n -->% of the operator time and <!-- n:cls.q6.filter -->78<!-- /n -->% of the samples are the filter pushed into it:
`EvalLogical` (<!-- n:frame.q6.EvalLogical.filter -->26.8<!-- /n -->%), `CompareFixed` (<!-- n:frame.q6.CompareFixed.filter -->19.7<!-- /n -->%), the copies around them (`_platform_memmove`
<!-- n:frame.q6._platform_memmove.filter -->14.2<!-- /n -->%, `memcpy` <!-- n:frame.q6.DYLD-STUB$$memcpy.filter -->5.6<!-- /n -->%) and `RepeatedArrayFactory::CreateBufferOf` (<!-- n:frame.q6.arrow::RepeatedArrayFactory::CreateBufferOf.filter -->3.6<!-- /n -->%), Arrow's
array-from-scalar helper, which the comparisons call for their literal operands. The query keeps
<!-- n:ea.q6.2.rows -->114,160<!-- /n --> of <!-- n:data.lineitem_rows -->6,001,215<!-- /n --> rows, so almost all the work is evaluating the plan's filter
(`bench/results/profile/explain-analyze-q6.txt`: four conjuncts, `BETWEEN` being one, joined by
three `AND`s) over rows that are then dropped. Row-group pruning cannot help on the default file,
because the dates are spread across every row group; on the `l_shipdate`-sorted file it can, and the
pruning table shows both effects.

### Q3

Q3's gap is the smallest, ratio <!-- n:main.q3.ratio -->3.59<!-- /n -->. <!-- n:cls.q3.Parquet decode -->24<!-- /n -->% of the samples are Parquet decode
(<!-- n:frame.q3.snappy::DecompressBranchless.Parquet decode -->17.4<!-- /n -->% in snappy decompression, which DuckDB also pays), <!-- n:cls.q3.probe -->36<!-- /n -->% are the probe
(`KeyIndex::Find` <!-- n:frame.q3.KeyIndex::Find.probe -->12.1<!-- /n -->%, `HashBytes` <!-- n:frame.q3.HashBytes.probe -->4.7<!-- /n -->%, `Gather` <!-- n:frame.q3.Gather.probe -->4.5<!-- /n -->%) and <!-- n:cls.q3.filter -->21<!-- /n -->% are
the filters pushed into the scans. No single frame stands out the way Q5's did before the fix.

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
| Q5 | HashJoin #6 | 226.7 | 39% | 1,201,113 | 204.6KiB |
| Q5 | HashJoin #3 | 129.3 | 22% | 7,243 | 32.9MiB |
| Q5 | HashJoin #5 | 92.9 | 16% | 182,183 | 19.6MiB |
| Q5, `order_joins` off | HashJoin #6 | 198.0 | 44% | 910,519 | 22.2MiB |
| Q5, `order_joins` off | Scan #7 | 80.4 | 18% | 6,001,215 | 0B |
| Q5, `order_joins` off | Scan #10 | 49.9 | 11% | 227,597 | 0B |
| Q6 | Scan #2 | 345.7 | 98% | 114,160 | 0B |
| Q6 | HashAggregate #1 | 8.5 | 2% | 1 | 281B |
| Q6 | Project #0 | 0.0 | 0% | 1 | 0B |
| Q5, before the join-estimate fix | HashJoin #3 | 3,871 | 73% | 7,243 | 3545.0MiB |
| Q5, before the join-estimate fix | HashJoin #5 | 986.9 | 19% | 18,435,748 | 691.2MiB |
| Q5, before the join-estimate fix | HashJoin #7 | 262.4 | 5% | 12,093,991 | 91.0KiB |

The three operators with the most own time in each `--explain-analyze` run on SF1 (`bench/results/profile/explain-analyze-*.txt`).
<!-- /bench:profile-operators -->

## Samples by category

<!-- bench:profile-categories -->
| Query | Parquet decode | filter | scan other | hash build | probe | aggregate | sort | other | samples |
|---|---|---|---|---|---|---|---|---|---|
| Q1 | 22% | 9% | 3% | 0% | 0% | 64% | 0% | 2% | 7,619 |
| Q3 | 24% | 21% | 3% | 7% | 36% | 6% | 0% | 2% | 7,625 |
| Q5 | 13% | 5% | 2% | 23% | 54% | 0% | 0% | 2% | 7,623 |
| Q5, `order_joins` off | 18% | 7% | 2% | 7% | 60% | 4% | 0% | 2% | 7,651 |
| Q6 | 15% | 78% | 4% | 0% | 0% | 3% | 0% | 1% | 7,592 |
| Q5, before the join-estimate fix | 2% | 1% | 0% | 72% | 25% | 0% | 0% | 0% | 7,578 |

Share of the samples taken on non-idle threads (`sample <pid> 10`, 1 ms interval) while `quarry query --repeat` looped the query on SF1; `bench/classify_samples.py` explains the categories. Source: `bench/results/profile/classified.json` (the row before the fix: `bench/results/profile/before-join-estimate-fix/classified.json`).
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
| Q5 | `quarry::KeyIndex::Find` | probe | 28.1% |
| Q5 | `snappy::DecompressBranchless` | Parquet decode | 9.6% |
| Q5 | `_platform_memmove` | probe | 4.8% |
| Q5 | `quarry::HashBytes` | probe | 4.8% |
| Q5 | `quarry::Gather` | probe | 3.4% |
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
| Q5, before the join-estimate fix | `quarry::KeyIndex::Find` | hash build | 27.2% |
| Q5, before the join-estimate fix | `quarry::KeyIndex::Insert` | hash build | 16.2% |
| Q5, before the join-estimate fix | `quarry::Gather` | probe | 9.2% |
| Q5, before the join-estimate fix | `_platform_memmove` | hash build | 7.4% |
| Q5, before the join-estimate fix | `quarry::KeyIndex::Find` | probe | 4.2% |
<!-- /bench:profile-frames -->
