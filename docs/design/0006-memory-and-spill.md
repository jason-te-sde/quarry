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
flight, or the buffers of a spill writer. `peak_state_bytes` in EXPLAIN ANALYZE is what was charged.

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
`SpillWriter::Open` and `SpillReader::Open` take the hook itself (`SpillFault`), not the context.

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
| input slice with a limit | 128 rows | a table grows by one slice before it is charged, so the overshoot stays small next to the budget |
| margin | 256 KiB | what operators may hold beyond the budget; see below |

### The margin

`peak_state_bytes` can never exceed the budget, since a failed `TryGrow` charges nothing. What can
exceed it is memory the budget does not see: the writers' pending rows (up to 4 KiB plus one batch's
share of that partition, for each of up to 16 files, twice while a partition is being split),
one input batch in flight, and the copy that concatenating the build rows makes. `TpchBudgetTest`
checks `peak_state_bytes <= budget + 256 KiB` for every operator of every run; the bound is slack by
construction, and the 256 KiB is the room these uncharged buffers get on the TPC-H shapes (rows of
tens of bytes). Rows hundreds of bytes wide make the writers' pending rows larger, in proportion to
row width times batch size.

## Consequences

- **Several spilling operators share one budget.** A join whose probe input is another join (a
  left-deep plan) runs with both tables live. If the outer table fills the budget, the inner one
  has no room and fails at depth 3 although it would fit alone. The optimizer's right-deep plans
  (the TPC-H ones) release an inner table before the outer one starts probing. A per-operator share
  of the budget is the fix, and is not done.
- **Skew fails instead of degrading.** Equal keys cannot be split; the error says so, and
  `docs/operations.md` says what to do.
- **A cancelled or timed-out spilling query ends promptly.** The loops that run inside one `Next`
  (splitting, merging, loading a partition) call `CheckInterrupted` themselves.

## Alternatives

- **Spill raw rows in the aggregate.** Simpler (no state merge), larger on disk when rows outnumber
  groups, and a partition of one heavy group would not need to fit at all. Rejected for the size.
- **Sort-merge join for the spilled case.** Needs an external sort, which does not exist.
- **Abort the budget when reservations outlive it.** See above.

## The deliberate flaw

`-DQUARRY_FLAW=spill_drops_last_batch` makes `SpillWriter::Finish` skip writing its buffered rows, so
the tail of every spilled partition is lost. `TpchBudgetTest` fails under it (Q5 at 64 KiB returns no
rows), and so do the spill and high-cardinality aggregate tests.
