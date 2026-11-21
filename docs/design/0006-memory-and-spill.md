# ADR 0006: The memory budget and spilling

Status: accepted.

## Context

A query engine that holds every hash table and sort buffer in memory fails on the first input larger
than the machine. quarry takes a limit (`--memory-limit`, `EngineOptions::memory_limit_bytes`, 0 for
none) and has to do one of three things when an operator's state would pass it: spill to disk and
carry on, fail with a message that says what to change, or, for the few operators that need all their
rows at once, fail cleanly. It must never be killed by the operating system and never return a wrong
answer because part of a spilled partition went missing.

## Decision

### What is charged

Only operator state is charged to the budget: the join's build rows and hash table, the aggregate's
group table and states, the sort buffer, the Top-N buffer. Scan buffers are not (a scan holds one
row group; the profile reports it as `buffer=`), and neither are output batches, one input batch in
flight, or the buffers of a spill writer. `peak_state_bytes` in EXPLAIN ANALYZE is what was charged;
what was allocated is `pool_peak` (see "The margin").

The storage of the hash tables themselves (the key index, the key arena and its offsets, every
aggregate state vector, the join's chains) is allocated from the query's `arrow::MemoryPool`
through `arrow::stl::allocator` (`src/exec/pool_vector.h`), so the pool peak measures it and a defect
that retains it shows up there. MIN and MAX of a VARCHAR keep their strings in `std::string`s, which
are charged but allocated outside the pool; they are the one exception.

**A growth is charged before it is allocated.** A rehash allocates the doubled slot array while the
old one still exists, and a vector that grows holds the old storage and the new at once. Every table
asks, before it grows, for the bytes the new storage will take (`KeyIndex::GrowthBytes`, the
`GrowthBytes` of each state), under one capacity rule (`GrownCapacity`) so that the prediction is
what the growth then allocates. A spillable aggregate that cannot pay stops at the group that needs
the growth, returns `false` with nothing allocated, is spilled and emptied, and is given the same
rows again; a join does the same with the slice of its build side, which goes to the partition
files. An empty table that cannot pay takes the rows anyway, since there is nothing to spill. The
charge made in advance is exact afterwards: once the old storage is freed the table's charge is
reduced to what it holds.

Every stateful operator owns a `Reservation` on the query's `MemoryBudget`. `Grow` past the limit
is `OutOfMemory("resource exhausted: <owner> needs <n> bytes, budget <limit>, in use <used>")`;
`TryGrow` returns false instead, which is what a spilling operator uses.

### A reservation may outlive its budget

Operators are destroyed after `Close`, and the budget lives in the engine's stack frame; the rule
"an operator must not outlive the budget it reserved from" was a comment. It is now structural:
the counters live in a shared state object that the budget and every reservation hold, so a
reservation destroyed after its budget returns its bytes to memory that is still alive. The other
option, a budget that aborts on destruction with reservations outstanding, only checks the order in
debug builds and turns a lifetime slip into a crash; sharing removes the slip in every build for
one reference count per reservation. `MemoryBudgetTest.AReservationMayOutliveItsBudget` frees the
budget first; under AddressSanitizer it is a heap-use-after-free without the shared state.

### Spill files

Spill files use the Arrow IPC stream format, uncompressed, one file per partition, in a directory
that exists for one query: `SpillDirectory::Create(parent)` makes `quarry-spill-XXXXXX` under
`EngineOptions::spill_parent` (the system temporary directory when empty) and its destructor removes
it recursively. The engine owns it for the length of `Execute`, so an error, a cancel or a timeout
leaves nothing behind. It is created only when a limit is set.

A `SpillWriter` buffers rows until 4 KiB are pending and then writes one record batch, so that one
partition's share of an input batch (a few dozen rows) does not become a record batch of its own;
`Finish` writes the rest. A partitioned spill keeps up to 16 writers open, which is why the buffer is
small: the buffers are not charged. I/O sites call `ExecContext::fault` (`spill.write`,
`spill.finish`, `spill.read`) so that tests can inject failures. `io` may not include `exec`, so
`SpillWriter::Open` and `SpillReader::Open` take the hook itself (`SpillFault`), not the context. <!-- const: the writer's flush size in src/io/spill_file.cc -->

### Grace hash join

The build side goes into the hash table as long as `TryGrow` succeeds. At the first failure the join
becomes a grace hash join:

1. the build rows read so far, and every later one, are written to 16 files by 4 bits of the hash of
   the join key; the table is emptied and its charge returned;
2. the probe side is read to the end and split the same way;
3. the pairs of files are joined one at a time: the build file goes into the table, the probe file
   streams through it. A pair with no rows on one side is dropped without being read.

A build file that does not fit is split again on the next 4 bits of the hash, and its probe file with
it, up to depth 3 (so 4 splits, 65,536 leaf partitions at most). A partition still too large at depth
3 fails with `resource exhausted: HashJoin #n needs ..., budget ..., in use ...: a partition does not
fit although the input was split by hash down to partition depth 3`. That happens when the rows share
a join key (or most of their hash): no hash split can separate equal keys.

Rows with a NULL key match nothing in an inner join and are dropped when a side is split.

### Hash aggregate

The group table is charged after every update. At the first `TryGrow` failure the operator writes the
**partial states** of all groups (the key, and per aggregate: COUNT the count, SUM the sum, AVG the
exact sum and the count, MIN/MAX the value) to 16 files by the hash of the key, clears the table and
carries on. At the end the remaining groups are written too; then each file is merged into a fresh
table with the same rule: a file whose groups do not fit is split on the next 4 bits (depth 3 is the
limit), a file that fits is finished and its groups handed out. Merging partial states instead of
spilling raw rows keeps the spill proportional to the number of groups, not of rows, and AVG still
divides once, at the end, so the result is the one an unlimited run gives. Group order is not
defined, as without a limit.

An aggregate without GROUP BY has one group and never spills in practice.

### Sort and Top-N

A sort needs all its rows to order them. It charges with `Grow` and an over-budget sort fails with
`resource exhausted: Sort #n needs <bytes> bytes, budget <limit>, in use <used>`. An external merge
sort is the obvious extension and is not in v0.1.0. A Top-N holds the best n rows and the batches not
yet merged into them; when the budget has no room for another batch it merges early, so its state is
about n rows plus one batch and it never spills.

### Defaults

| Constant | Value | Why |
| --- | --- | --- |
| partitions per split | 16 | enough to get a 1 GiB build side under a 64 MiB budget in one split, few enough that 16 open writers stay cheap |
| maximum depth | 3 | four splits separate 65,536 ways; deeper only helps when the hash is not the limit, and then it is key skew |
| writer flush size | 4 KiB | small, because 16 to 32 writers buffer at once and the buffers are uncharged |
| input slice with a limit | 128 rows | what an input slice carries into a table that the budget has not yet been asked about (the rows themselves, not the table's growth, which is charged first) stays small next to the budget |
| margin | 256 KiB | what the operators' pool may hold beyond the limit; see below |
<!-- const: the defaults of src/exec and the margin the tests assert -->

### The margin, and how it is measured

`peak_state_bytes` is the most the budget charged, and it can never pass the limit: a failed
`TryGrow` charges nothing. It says nothing about memory the budget does not see, so it is not the
bound. The bound is measured: every query runs with its own `arrow::ProxyMemoryPool` over the
default pool, `ExecContext::pool`, so that operator, spill-writer, spill-reader and concatenation
allocations, and the hash tables' storage, are counted whether or not they were charged. Its peak is
`QueryResult::pool_peak_bytes` and the last line of EXPLAIN ANALYZE (`pool_peak=`). The result table is
copied out of the pool as it is produced (it belongs to the caller, and a large result would
otherwise hide the operators' peak). Scans have a pool of their own (the Parquet reader allocates
from it), reported as `scan_pool_peak_bytes` and not included, as scan buffers are not charged
either.

**The stated margin is a function of what the pool holds beyond the charge.** For a hash aggregate
and a hash join, the operators' pool stays within `limit + 256 KiB`: the fixed part is the
spill-writer buffers (up to 4 KiB plus one batch's share of that partition, for each of up to 16
files, twice while a partition is being split), one input batch and its evaluated keys, and
expression temporaries. Rows hundreds of bytes wide scale the writer buffers with row width times
batch size; the margin is not a guarantee for them. The part that used to grow with the table (the
slot array of a rehash, the arena and the state vectors, each held twice while it was copied) is
charged before it is allocated, so it is no longer in the margin. The whole process is a different
sum, and two parts of it are outside the pool bound: <!-- const: the margin is the constant asserted by the tests named below -->

- **One row group per scan**, allocated from the scan pool: not charged, reported separately.
- **A copy that is made while the original is alive.** A join keeps its build rows as the batches the
  scan produced (in the scan pool) and concatenates them into one array per column (in the operator
  pool); the budget charges one copy, so for that moment the process holds up to the build payload
  twice. The sort concatenates in the operator pool, so there the pool itself holds its input twice.
  The first is measured by the wide-join case below (the pool peak and the scan pool side by side);
  the second is read from the code and not measured.
<!-- const: the margin and the limits named here are those of tests/unit/exec/near_limit_test.cc and tests/tpch/tpch_budget_test.cc, which assert them -->

`TpchBudgetTest`, `HighCardinalityAggTest` and `NearLimitTest` assert `pool_peak_bytes <= limit + 256
KiB`. Measured on SF0.01 with the dev build (`--explain-analyze` prints `pool_peak=`):

| Query | budget | pool_peak | scan pool |
| --- | --- | --- | --- |
| Q1 | 64 KiB (also 1 MiB, unlimited) | 132.5 KiB | 321.3 KiB |
| Q3 | 64 KiB / 1 MiB / unlimited | 127.2 / 164.3 / 164.3 KiB | 230.6 KiB |
| Q5 | 64 KiB / 1 MiB / unlimited | 153.8 / 355.1 / 355.1 KiB | 225.6 KiB |
| Q6 | any | 33.5 KiB | 216.6 KiB |
| `GROUP BY l_orderkey` | 64 KiB / unlimited | 110.8 KiB / 1.1 MiB | 96.0 KiB |
<!-- obs: read from `quarry query --explain-analyze` on tests/data/tpch-sf0.01 with the dev build while the near-limit test was written; no results file holds them, the tests assert the bound -->

The aggregate's pool peak at no limit was 48.2 KiB before its state was in the pool; it is now
1.1 MiB. The worst overshoot at 64 KiB is Q1's 132.5 KiB, which is 67 KiB beyond the budget: most of
it is expression temporaries, not state.
<!-- obs: the 48.2 KiB is the earlier row of this table, read before the state was routed through the pool; the rest is from the table above -->

`NearLimitTest` drives the same check where a hash table grows until it reaches the limit: an
aggregate of 600,000 groups (`k`, `SUM`, `COUNT`, `AVG`, `MIN`), a join with a build side of 500,000
rows of four columns, and a join whose 16,000 build rows are 400 bytes wide (about 6.4 MiB of
payload under an 8 MiB limit). Dev build; the pool peak is what the assertion is on: <!-- const: the sizes of the test's data -->

| Query | limit | pool peak | beyond the limit | scan pool | spilled |
| --- | --- | --- | --- | --- | --- |
| aggregate | unlimited | 110.50 MiB | - | 0.05 MiB | no |
| aggregate | 1 MiB | 0.54 MiB | -0.46 MiB | 0.05 MiB | yes |
| aggregate | 16 MiB | 7.72 MiB | -8.28 MiB | 0.05 MiB | yes |
| aggregate | 64 MiB | 30.78 MiB | -33.22 MiB | 0.05 MiB | yes |
| aggregate | 128 MiB | 61.50 MiB | -66.50 MiB | 0.05 MiB | yes |
| join | unlimited | 11.48 MiB | - | 11.65 MiB | no |
| join | 1 MiB | 0.11 MiB | -0.89 MiB | 0.24 MiB | yes |
| join | 16 MiB | 0.74 MiB | -15.26 MiB | 3.10 MiB | yes |
| join | 64 MiB | 11.48 MiB | -52.52 MiB | 11.65 MiB | no |
| wide join | unlimited | 6.70 MiB | - | 6.50 MiB | no |
| wide join | 2 MiB | 0.64 MiB | -1.36 MiB | 2.08 MiB | yes |
| wide join | 8 MiB | 6.70 MiB | -1.30 MiB | 6.50 MiB | no |
| wide join | 16 MiB | 6.70 MiB | -9.30 MiB | 6.50 MiB | no |
<!-- obs: printed by NearLimitTest (dev build); no results file holds them, the test asserts the bound -->

Two things the table shows. The aggregate spills at 128 MiB although its unlimited peak is 110.5
MiB: paying for the doubling of the slot array before it is allocated counts the old and the new
array together, so the table is refused earlier than its final size suggests, and the pool never
reaches the limit. And the wide join, which does not spill at 8 MiB, holds 6.70 MiB in the operator
pool and 6.50 MiB in the scan pool at once: the build payload twice, 13.2 MiB for a limit of 8 MiB,
which is the second bullet above. The pool bound holds; the process bound at a large limit is
about twice the limit for a join and a sort, and `docs/operations.md` says so.
<!-- obs: read off the table above -->

The measurement is itself tested: `-DQUARRY_FLAW=spill_retains_rows` makes the spill keep a copy of
every row it writes, outside any reservation. Answers stay correct and `peak_state_bytes` does not
move, but the pool peak of Q3 and Q5 at 64 KiB becomes 1.1 and 2.2 MiB and `TpchBudgetTest` fails.
<!-- obs: measured when the flaw was added; not repeated after the hash tables moved into the pool -->
A defect that retained hash-table storage would now be seen as well: `NearLimitTest` asserts that the
unlimited aggregate's pool peak is above 16 MiB, which fails if the state is allocated elsewhere.

## Consequences

- **Several spilling operators share one budget.** A join whose probe input is another join runs
  with both tables live, and a join whose build input is a join keeps its own table live next to the
  ones below it. If the outer table fills the budget, the inner one has no room and fails at depth 3
  although it would fit alone. The TPC-H plans are not all right-deep: in Q5's shipped plan
  (`bench/results/profile/explain-analyze-q5.txt`) `HashJoin #6` is the probe side of `HashJoin #5`,
  which is the build side of `HashJoin #3`, so while #3 builds, the tables of #5 (peak
  <!-- n:ea.q5.5.peak -->19.6MiB<!-- /n -->) and #6 (<!-- n:ea.q5.6.peak -->204.6KiB<!-- /n -->) are live too. Q5 works at a small limit because the table that is
  live next to the large one is small. A per-operator share of the budget is the fix, and is not
  done.
- **Skew fails instead of degrading.** Equal keys cannot be split; the error says so, and
  `docs/operations.md` says what to do.
- **A cancelled or timed-out spilling query ends promptly.** The loops that run inside one `Next`
  (splitting, merging, loading a partition) call `CheckInterrupted` themselves.

## Alternatives

- **Spill raw rows in the aggregate.** Simpler (no state merge), larger on disk when rows outnumber
  groups, and a partition of one heavy group would not need to fit at all. Rejected for the size.
- **Sort-merge join for the spilled case.** Needs an external sort, which does not exist.
- **Abort the budget when reservations outlive it.** See above.

## The deliberate flaws

`-DQUARRY_FLAW=spill_drops_last_batch` makes `SpillWriter::Finish` skip writing its buffered rows, so
the tail of every spilled partition is lost. `TpchBudgetTest` fails under it (Q5 at 64 KiB returns no
rows), and so do the spill and high-cardinality aggregate tests. `spill_retains_rows` is the second
one, described under the margin.
 <!-- const: the flaw's test parameters -->