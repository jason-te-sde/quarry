# ADR 0007: Scope: what quarry does not do

Status: accepted.

## Context

quarry exists to show one thing: a vectorized engine whose answers are checked against DuckDB and
whose speed is explained. Every feature added to a SQL engine costs code in every layer and tests in
every layer, and a feature that the four benchmark queries never touch adds cost without adding to
that demonstration. So the project draws its line early and writes the line down, together with the
reason for each item, so that a reader can tell a deliberate omission from a gap nobody noticed.

What quarry does is in [docs/sql.md](../sql.md) (the SQL subset), [ADR 0006](0006-memory-and-spill.md)
(memory and spilling) and the other ADRs. This page is the other side of that line.

## Decision

quarry does not do the following. Where an item is a piece of SQL, the lexer reserves the word or the
parser refuses the construct with a message that names it, so that using it is an error that says
"not supported" and not a confusing failure further in.

| Not done | Why | What adding it would touch |
| --- | --- | --- |
| Outer, semi and anti joins | The four target queries use inner joins only. An outer join changes what a join means for rows without a partner, which the hash join, the join-order rule and the cardinality estimate all assume away. | `LEFT`, `RIGHT`, `FULL` and `OUTER` are reserved words in [lexer.cc](../../src/parser/lexer.cc); the grammar in [parser.cc](../../src/parser/parser.cc) and the join node in [logical.h](../../src/plan/logical.h) need a join type; [rules.cc](../../src/planner/rules.cc) must stop treating every join as freely reorderable; [hash_join_op.cc](../../src/exec/hash_join_op.cc) must emit unmatched rows, and the spilled path must do the same. |
| Subqueries, CTEs (`WITH`), `EXISTS` | They need a binder that can resolve a name in an enclosing query, and a planner pass that turns a correlated subquery into a join. Neither is small, and none of the target queries has one. | A scope stack in [binder.cc](../../src/binder/binder.cc), a decorrelation pass in the planner, and the semi and anti joins of the row above. |
| Window functions (`OVER`) | A different operator (partition, sort, evaluate a frame) and a different place in the plan. | A new parser production, a binder step after aggregation, a new operator in [src/exec](../../src/exec) that needs sorted, partitioned input. |
| `UNION`, `INTERSECT`, `EXCEPT` | Set operators need output-type reconciliation between branches and, for the non-`ALL` forms, a distinct. | A statement-level node in the grammar and the logical plan, and a type-merge rule next to [type_rules.cc](../../src/types/type_rules.cc). |
| `DISTINCT` | It is a grouping with no aggregate; it is cheap to add and was left out only to keep the aggregate paths (and their spilling and tests) to one shape. `COUNT(DISTINCT x)` is the harder half: it needs a per-group set. | The binder could rewrite `SELECT DISTINCT` to a `GROUP BY` over the select list; `COUNT(DISTINCT)` needs new aggregate state in [agg_hash_table.cc](../../src/exec/agg_hash_table.cc) that spills. |
| `LIKE`, `ILIKE`, `CASE` | They are expression kernels over strings and booleans, and none of the four target queries uses one. A string matcher that agrees with DuckDB on every input is a project of its own. | A kernel in [src/expr](../../src/expr), a type rule, and generator support in the differential test so that it is compared and not just implemented. |
| Division (`/`) | The result type of a decimal division is a rule of its own, and DuckDB's rule has to be reproduced exactly for the differential test to pass. `AVG` is the one division the target queries need, and it is an aggregate. | A type rule in [ADR 0004](0004-decimal-and-type-rules.md) and [type_rules.cc](../../src/types/type_rules.cc), a kernel in [eval_arith.cc](../../src/expr/eval_arith.cc) with a division-by-zero decision, and a place in the grammar. |
| Multi-threaded execution (morsel parallelism) | One thread keeps "what does vectorization buy" and "where does the time go" answerable: there is one stack to sample and no scheduler to blame. The comparison is therefore against single-threaded DuckDB, and says so. Parallelism is a candidate for the next version. | The pull model of [ADR 0002](0002-pull-based-batches.md) would give way to, or be wrapped by, a scheduler; every hash table and the memory budget would need to be safe under concurrent use; the profile would need per-thread time. An `Engine` already refuses a second concurrent query by construction. |
| Writing a Parquet decoder | The project is about execution, not file formats. Decoding is Arrow's, so its cost is quarry's cost, and [where-time-goes.md](../where-time-goes.md) lists it as its own category. | A reader in [src/io](../../src/io) that stays inside the boundary of [ADR 0003](0003-arrow-boundary.md). |
| Writing data (`INSERT`, `UPDATE`, `COPY ... TO`) | quarry is a read-only engine over files that other tools write. | A catalog that can change, a Parquet writer, and a story for what a running query sees. |
| Statistics collection (histograms, distinct counts, sampling) | The estimator uses only what the Parquet footer gives for free: row counts and per-row-group min/max, plus fixed selectivities ([ADR 0005](0005-optimizer.md)). Collecting more means reading data when a table is registered. The known failure cases of the estimator are listed in that ADR, and one of them is the remaining Q5 join-order gap in [where-time-goes.md](../where-time-goes.md). | A statistics pass or a stored statistics file, and a rewrite of [cardinality.cc](../../src/planner/cardinality.cc). |
| External sort | Only joins and aggregations spill. A sort that exceeds the memory limit fails with `resource exhausted` and says what it needed; `ORDER BY ... LIMIT` uses a bounded heap and does not. Joins and aggregations were the spilling that the target queries need, and a merge sort over spilled runs is a third spill path to test. | A run writer and a k-way merge in [src/exec](../../src/exec), reusing the Arrow IPC spill files of [ADR 0006](0006-memory-and-spill.md). |
| JIT and code generation | Interpreting vectorized kernels is the thing this project shows; generating code is another project with another set of trade-offs, written up in [ADR 0001](0001-vectorized-interpretation.md). | A different execution backend behind the physical plan. |
| A network server, sessions, authentication | quarry is an embedded library plus a command-line tool. | Everything. |
| Windows | Linux and macOS only: the build script, the spill directory handling and the profiling procedure assume a POSIX system, and the CI matrix has no Windows runner. | Path handling in [src/io](../../src/io), `scripts/build_arrow.sh`, and a CI job. |

Two refusals are smaller than the rest and are limits of the SQL subset, not of the engine: `SELECT *`
(list the columns) and ordinals in `GROUP BY` and `ORDER BY` (name the column). Both are in
[docs/sql.md](../sql.md).

## Consequences

- A reader sees what is missing before they find out by trying. The README says what the test suite
  does not cover; this page says what the engine does not do.
- A query outside the subset fails at parse or bind time with a message that names the construct,
  never by returning a wrong answer. The differential test's generator stays inside the subset on
  purpose ([docs/testing.md](../testing.md)), so a difference from DuckDB is a bug in quarry and not an
  unsupported feature.
- The comparison with DuckDB is narrower than the engines are. quarry is not meant to replace a
  general-purpose database, and the benchmark makes no claim outside the four queries it runs.

## Alternatives

- **Implement a wider SQL subset.** Rejected for v0.1: each item above multiplies the test and
  differential-generator work, and the result would be a thinner version of a wider engine, not a
  deeper one of a narrower engine.
- **Leave the list unwritten and let the SQL documentation imply it.** Rejected: an unwritten scope
  looks like an oversight to a reader who does not know the history.
