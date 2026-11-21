# Operations

How to run quarry with a memory limit, where it puts files, and what its errors mean.

## Picking a memory limit

`--memory-limit SIZE` (`EngineOptions::memory_limit_bytes`) bounds the state operators hold: hash
join tables, aggregate groups, sort and Top-N buffers. The hash tables' own storage (the key index,
the key arena, the aggregate states and the join chains) is allocated from the query's memory pool, so
`--explain-analyze` and `QueryResult::pool_peak_bytes` measure it, and a table pays for a growth,
including the doubled slot array of a rehash, before it allocates it. The limit does not bound scan
buffers (one row group per scan, shown as `buffer=` in `--explain-analyze`), output batches, or the
process's other memory. `0` or no flag means unlimited and never touches the disk.

What to count on, in three parts:

- **The operators' pool** stays within the limit plus 256 KiB <!-- const: the margin of ADR 0006, asserted by the near-limit test; the limits named below are that test's --> for the state that the budget charges, in a hash aggregate and a hash join: the hash tables, spill-writer buffers, one input batch and expression temporaries. `tests/unit/exec/near_limit_test.cc` runs a high-cardinality aggregate and
  joins at 1, 16 and 64 MiB (and an aggregate at 128 MiB) and asserts it; the measured peaks are in
  [ADR 0006](design/0006-memory-and-spill.md#the-margin-and-how-it-is-measured). **This bound does not
  hold for a join's build side.** The join concatenates its build rows into one array per column, and
  the budget charges one copy of them. The copy that is not charged is in the operator pool when the
  build input is filtered, is the output of another join or aggregate, or is read back from a spill
  file; only for a build input that is a bare, unfiltered scan do the batches being copied live in the
  scan's pool. The pool can then pass the limit by up to the payload of the build partition that is in
  memory, and above 2^20 build rows by up to twice that, because the concatenation holds the parts,
  the pieces and the final array of a column at once. Measured: Q5 at `--memory-limit 16MB` peaks at
  <!-- n:spill.q5.16MB.state -->17.09<!-- /n --> MB in the operator pool (the README's spill table). The test with a filtered build side
  pins the overshoot as it is. It is a known issue (CHANGELOG), and charging the concatenation, or releasing the parts as they are copied, would remove it.
- **One row group per scan** comes on top. It is allocated from the scan's own pool and is often tens
  of MiB.
- **A join's or a sort's input exists twice for a moment.** For a join whose build input is a bare
  scan, the build rows are the batches the scan produced, which live in the scan's pool, and the join
  copies them into one array per column in the operator pool before it releases them. The budget
  charges one copy, so while the copy is made the process holds the limit plus up to the size of the
  build payload; the near-limit test measures this for a join, with the pool and the scan pool side by
  side. For any other build input the second copy is in the operator pool, as the first bullet says.
  The sort compacts its input into the operator pool and concatenates it there, so the pool itself can
  hold the sorted input twice, up to twice the limit; this is read from the code and not measured. For
  an aggregate there is no copy.

- A join or aggregation that does not fit **spills** and is slower, not wrong: the answer is the
  same at every limit. Q5 at scale factor 1 takes <!-- n:spill.q5.16MB.ms -->8,124<!-- /n --> ms at `--memory-limit 16MB` against
  <!-- n:spill.q5.unlimited.ms -->587.8<!-- /n --> ms unlimited (the README's spill table).
- A **sort** over the limit fails. Give a sort enough room for its whole input (rows times row width,
  plus 4 bytes a row), or add a `LIMIT` so that the plan uses Top-N.
- Run `--explain-analyze` at a candidate limit and read `peak=` and `spilled=` per operator: an
  operator with `spilled=` wrote that many bytes to disk, in total, over every split.

A limit much smaller than the largest group table or build side makes the first split produce
partitions that are still too large and the second split rewrites them: `spilled=` several times the
input is the sign. Raise the limit until it drops.

## How promptly a timeout or Ctrl-C ends a query

The deadline and the cancel flag are checked once per batch an operator returns, inside the loops
that build a hash table, split a partition or merge spilled files, once per Parquet row group and
once per batch a scan reads (so a scan whose filter matches nothing can be interrupted, although it
returns no batch), and between pieces of 2^20 rows (or 65,536 batches) when a hash join, sort or
top-N joins its materialised input into one column. They are not checked inside the read of one
Parquet row group, inside the single copy that joins those pieces, while the operators release their
state after the query ended (on a self-join of 2 million one-row arrays, about 1 GB of state, the
process ended 51 ms after a 1 s deadline and 167 ms after a 5 s one: the delay grew with the state;
not measured at larger sizes) <!-- obs: timed by hand once on SF0.01-sized test data while the timeout tests were written; no results file -->,
or while the operating system pages memory in and out: a query that has outgrown physical memory
can overrun its timeout by however long one such step takes under swapping. With a
very small `--batch-size` the build side of a join is one array per row and its memory is far
larger than the budget charges for it, which is how that happens (the first ablation attempt, with
batch size 1 on Q5 before the join-estimate fix, swapped and was killed; `benchmarks.md` records it).
A second Ctrl-C ends the `quarry` process at once: the first one restores the default signal action.

## Where spill files go

In a directory `quarry-spill-XXXXXX` created under `EngineOptions::spill_parent`, or the system
temporary directory (`$TMPDIR`, else `/tmp`) when that is empty. The directory is created when a
query starts with a limit and removed, with everything in it, when the query ends: success, error,
Ctrl-C (SIGINT cancels the query and the CLI exits 130 after the directory is gone) or timeout.
Only a process killed with SIGKILL, or a power loss, leaves one behind; delete the old
`quarry-spill-*` directories by hand.

The files hold Arrow IPC streams, uncompressed. A query can need disk for every row of its spilled
inputs once per split, so budget disk at a few times the size of the joined and grouped columns.
Q5 at scale factor 1 with `--memory-limit 16MB` writes <!-- n:spill.q5.16MB.spilled -->128.18<!-- /n --> MB (the README's spill table).

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

1. Look at the key's distribution:
   `SELECT key, COUNT(*) AS n FROM t GROUP BY key ORDER BY n DESC LIMIT 5`.
2. If the heavy value should not join (a placeholder), filter it out of the build side:
   `WHERE key <> 0`.
3. If it must join, give the join enough room for that partition: the build rows of the heavy key
   plus their chain, 4 bytes plus the payload per row, plus the writer buffers. <!-- const: sizes from the code (a uint32 chain entry; ADR 0006's 4 KiB writer buffers) -->
4. Make the side with the heavy key the probe side, which streams: put the other table first, or
   switch `pick_build_side` off (`--disable-rule pick_build_side`) and write the larger table on
   the left of the join.

A heavy **grouping** key does not fail: its partial states merge to one state however many rows it
had.

### Several joins and a small limit

Joins whose probe side is itself a join keep both tables in memory at once. If a limit is tight and
the message names the inner join at depth 3, raise the limit: the outer table is probably holding the
room the inner one needs.
