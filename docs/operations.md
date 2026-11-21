# Operations

How to run quarry with a memory limit, where it puts files, and what its errors mean.

## Picking a memory limit

`--memory-limit SIZE` (`EngineOptions::memory_limit_bytes`) bounds the state operators hold: hash
join tables, aggregate groups, sort and Top-N buffers. It does not bound scan buffers (one row group
per scan, shown as `buffer=` in `--explain-analyze`), output batches, or the process's other
memory. Count on the process using the limit plus a few MiB plus one row group per scan. `0` or no
flag means unlimited and never touches the disk.

- A join or aggregation that does not fit **spills** and is slower, not wrong: the answer is the
  same at every limit. Q5 on SF0.01 at 64 KiB takes about three times as long as unlimited (debug build).
- A **sort** over the limit fails. Give a sort enough room for its whole input (rows times row width,
  plus 4 bytes a row), or add a `LIMIT` so that the plan uses Top-N.
- Run `--explain-analyze` at a candidate limit and read `peak=` and `spilled=` per operator: an
  operator with `spilled=` wrote that many bytes to disk, in total, over every split.

A limit much smaller than the largest group table or build side makes the first split produce
partitions that are still too large and the second split rewrites them: `spilled=` several times the
input is the sign. Raise the limit until it drops.

## How promptly a timeout or Ctrl-C ends a query

The deadline and the cancel flag are checked once per batch an operator returns, inside the loops
that build a hash table, split a partition or merge spilled files, and between pieces of 2^20
rows (or 65,536 batches) when a hash join, sort or top-N joins its materialised input into one
column. They are not checked inside one Parquet row group read, inside the single copy that joins
those pieces, while the operators release their state after the query ended (on a self-join of 2 million one-row
arrays, about 1 GB of state, the process ended 51 ms after a 1 s deadline and 167 ms after a 5 s one:
the delay grew with the state; not measured at larger sizes), or while the operating system pages memory in and out: a query that has
outgrown physical memory can overrun its timeout by however long one such step takes under swapping. With a
very small `--batch-size` the build side of a join is one array per row and its memory is far
larger than the budget charges for it, which is how that happens (the first ablation attempt, with
batch size 1 on Q5 before the join-estimate fix, swapped and was killed; `benchmarks.md` records it).

## Where spill files go

In a directory `quarry-spill-XXXXXX` created under `EngineOptions::spill_parent`, or the system
temporary directory (`$TMPDIR`, else `/tmp`) when that is empty. The directory is created when a
query starts with a limit and removed, with everything in it, when the query ends: success, error,
Ctrl-C (SIGINT cancels the query and the CLI exits 130 after the directory is gone) or timeout.
Only a process killed with SIGKILL, or a power loss, leaves one behind; delete the old
`quarry-spill-*` directories by hand.

The files hold Arrow IPC streams, uncompressed. A query can need disk for every row of its spilled
inputs once per split, so budget disk at a few times the size of the joined and grouped columns.
Q5 at 64 KiB on SF0.01 writes about 2.3 MiB.

## What the errors mean

All three are `OutOfMemory` with the prefix `resource exhausted: `.

| Message | Cause | What to do |
| --- | --- | --- |
| `resource exhausted: Sort #n needs <bytes> bytes, budget <limit>, in use <used>` | a sort cannot spill | raise the limit, or use `ORDER BY ... LIMIT n` |
| `resource exhausted: TopN #n needs ...` | n rows plus one batch do not fit | raise the limit or lower n |
| `resource exhausted: HashJoin #n` or `HashAggregate #n needs ..., budget ..., in use ...: a partition does not fit although the input was split by hash down to partition depth 3 ...` | skew: rows that share a key (or most of its hash) cannot be separated by splitting on the hash | see below |

### Skew

A join key that one value dominates (a million rows of `NULL`-like default key, `0`, `-1`, or an
empty string) puts all of those rows in one partition at every depth. The message names the operator
and the depth.

1. Look at the key's distribution: `SELECT key, COUNT(*) FROM t GROUP BY key ORDER BY 2 DESC LIMIT 5`.
2. If the heavy value should not join (a placeholder), filter it out of the build side:
   `WHERE key <> 0`, or turn it into NULL, which never matches.
3. If it must join, give the join enough room for that partition: the build rows of the heavy key
   plus their chain, roughly 12 bytes plus the payload per row, plus the 4 KiB buffers.
4. Make the side with the heavy key the probe side, which streams: put the other table first, or
   switch `pick_build_side` off (`--disable-rule pick_build_side`) and write the larger table on
   the left of the join.

A heavy **grouping** key does not fail: its partial states merge to one state however many rows it
had.

### Several joins and a small limit

Joins whose probe side is itself a join keep both tables in memory at once. If a limit is tight and
the message names the inner join at depth 3, raise the limit: the outer table is probably holding the
room the inner one needs.
