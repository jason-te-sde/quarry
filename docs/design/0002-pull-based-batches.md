# ADR 0002: Pull-based batches

Status: accepted. The measured cost is added after the benchmarks of the ablation study.

## Context

A physical plan is a tree of operators and the data has to move between them. There are two common
shapes.

- **Push.** The scan drives. It reads a batch and calls `Consume(batch)` on its parent, which calls
  its own parent, and so on up to the output. A pipeline (a scan, the filters and projections over
  it, and the probe side of a join) is one chain of calls, and an operator that has to see all of its
  input (a hash table build, a sort) ends a pipeline and starts the next one. This is DuckDB's model.
  It keeps the working set of a pipeline in the cache and has no per-batch virtual call on the way
  up, but control flow is inverted: an operator cannot decide when it wants more.
- **Pull.** The consumer drives. The root calls `Next()` on its child, which calls `Next()` on its
  own child, until a scan returns a batch. Control flow is the one of an ordinary iterator.

## Decision

quarry's operators are pull-based. `Operator::Next()` returns one batch, or `std::nullopt` at the end
of the stream; a batch with no live rows is never returned (a filter that drops a whole batch pulls
the next one).

What the pull model makes easier here:

- **Short circuits.** `LIMIT n` stops asking for batches once it has n rows, and the scan below it
  stops reading row groups. Nothing has to be unwound: the operator simply does not call `Next()`
  again. The same holds for a hash join whose build side turns out empty, which never pulls the
  probe side (the test `AnEmptyBuildSideGivesNoRowsAndLeavesTheProbeUnread` checks it).
- **Cancellation and timeouts.** Every operator is wrapped by one profiling decorator whose `Next()`
  looks at the cancel flag and the deadline first. That is one place, and a query stops at the next
  batch boundary of whichever operator is running, because every batch crosses a `Next()`.
- **Operators with a phase.** A sort, a Top-N, a hash aggregate and the build side of a join each
  read all of their input in the first `Next()` and then hand out results. In a pull model that is a
  `bool consumed_` and a loop; a push model needs a separate "finish" callback and a pipeline graph.
  Spilling (a later task) is the same shape: the operator decides, while it drains its input, that it
  has to write partitions out.
- **Resumable output.** A hash join with duplicate keys can produce many output rows for one probe
  batch. The operator keeps the probe batch and a cursor, and returns `batch_size` pairs per call; a
  push model would have to call its consumer in a loop and could not pause.
- **Profiling.** The time of an operator is the time inside its own `Next()` minus the time inside
  its children's; with calls nested like this the subtraction is exact. A push chain would need the
  consumer's time subtracted instead.

What it costs:

- **One virtual call per batch per operator edge**, on the way down; push pays one on the way up, so
  the two have the same count. The difference is that a pull-based pipeline returns each batch
  through every stack frame, so a batch that is created by the scan and consumed by the aggregate
  makes a round trip of calls through the filter and the projection in between. The data does not
  move (the columns are shared pointers and a filter only sets a selection vector), but the return
  values of `arrow::Result<std::optional<Batch>>` are moved up the stack.
- **No pipeline-level optimisations.** A push engine can fuse a filter and a projection into the
  loop of the scan; quarry runs them as separate operators that each materialise their output. This
  is a cost of the batch-at-a-time design as much as of pulling, and it is the part that shows up in
  the profile.
- **No parallelism for free.** A push pipeline is a natural unit to run on several threads; a pull
  tree is one thread. quarry v0.1 is single-threaded by decision, so this is not paid yet.

How large the cost is: each `Next()` is a virtual call, a `Result` move, a clock read for the profile
and an atomic load for the cancel flag, paid once per batch, so it is paid per row only when the
batch size shrinks (at one row per batch the call overhead is the query). This record states no
figure for it. The batch-size ablation in [docs/benchmarks.md](../benchmarks.md#ablation-batch-size)
times every query at several batch sizes, and that table is the measurement.
