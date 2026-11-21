# Architecture

How a query travels through quarry, layer by layer, with the file that does each step and the test
that checks it. The example is TPC-H Q3 ([tests/tpch/queries/q3.sql](../tests/tpch/queries/q3.sql)): three
tables, two filters, two joins, a grouped sum and a top-10, which touches every layer.

## Layers

The layers are CMake targets, and `scripts/check-layering.sh` reads every `#include` and fails when
one points the wrong way. The drawing is simplified: the table in that script is the complete list of
who may include whom.

```
tools/quarry (CLI)
      |
   engine ---------------------------------------+
      |                                          |
  planner --> binder --> parser                  exec --> io
      |          |                               |        |
      +--------> plan <--------------------------+        |
                  |                                       |
                 expr --> vector --> types <--------------+
                                        |
                                      Arrow
```

| Layer | Directory | What it owns |
| --- | --- | --- |
| `types` | [src/types](../src/types) | `LogicalType`, DATE arithmetic, 64- and 128-bit DECIMAL, the result-type rules ([ADR 0004](design/0004-decimal-and-type-rules.md)) |
| `vector` | [src/vector](../src/vector) | `Batch` (columns plus an optional selection vector), `SelectionVector`, `Gather` |
| `expr` | [src/expr](../src/expr) | bound expressions and the vectorized evaluator |
| `parser` | [src/parser](../src/parser) | lexer, recursive-descent parser, syntax tree. No Arrow |
| `plan` | [src/plan](../src/plan) | logical and physical plan nodes (plain data) and `EXPLAIN` text |
| `binder` | [src/binder](../src/binder) | the catalog (tables and their Parquet metadata), name resolution, type derivation |
| `planner` | [src/planner](../src/planner) | the rewrite rules, join graph and cardinality estimates, then the physical plan |
| `io` | [src/io](../src/io) | Parquet metadata, spill files and the per-query spill directory |
| `exec` | [src/exec](../src/exec) | the operators, hash tables, memory budget and spilling |
| `engine` | [src/engine](../src/engine), [include/quarry/engine.h](../include/quarry/engine.h) | `quarry::Engine`, the one public class |

Arrow supplies memory (arrays, scalars), the Parquet reader and IPC for spill files. It does not
supply computation: no `arrow::compute` header is included and no compiled object references it
([ADR 0003](design/0003-arrow-boundary.md), checked by `scripts/check-layering.sh` and
`scripts/check-objects.sh`).

## One query, end to end

```sh
build/release/tools/quarry query --data tests/data/tpch-sf0.01 -f tests/tpch/queries/q3.sql
```

[tools/quarry_main.cc](../tools/quarry_main.cc) builds an `EngineOptions` from the flags, creates an
`Engine`, registers the directory, and calls `Execute`. Everything below happens inside it, in
[src/engine/engine.cc](../src/engine/engine.cc).

### 0. Registering the tables

`Engine::RegisterDirectory` makes every `x.parquet` file (or directory of files) the table `x`. The
[catalog](../src/binder/catalog.cc) reads each file's footer with `ReadParquetMetadata`
([src/io/parquet_scan.h](../src/io/parquet_scan.h)): the schema, the row count, and the minimum,
maximum and null count of every column in every row group. No data is read. A column of a type
quarry does not support still registers, marked unusable with the reason, so that the other columns
work. Tests: [catalog_test.cc](../tests/unit/binder/catalog_test.cc),
[parquet_metadata_test.cc](../tests/unit/io/parquet_metadata_test.cc).

### 1. Lexing and parsing

[lexer.cc](../src/parser/lexer.cc) turns the text into tokens with line and column. Words of SQL that
quarry does not implement (`LEFT`, `DISTINCT`, `CASE` and others) are reserved so that using one is an
error that says so ([ADR 0007](design/0007-scope.md)). [parser.cc](../src/parser/parser.cc) is a
recursive-descent parser for the grammar in [docs/sql.md](sql.md) and produces the tree of
[ast.h](../src/parser/ast.h). Q3 becomes one `SELECT` with four select items, three `FROM` items, a
`WHERE` of five conjuncts, three `GROUP BY` expressions, two sort keys and a limit. Nesting is
bounded, so the parser cannot overflow its stack on hostile input. Tests:
[lexer_test.cc](../tests/unit/parser/lexer_test.cc), [parser_test.cc](../tests/unit/parser/parser_test.cc),
[tests/sql/syntax.test](../tests/sql/syntax.test), and the `fuzz_parser` target in [fuzz/](../fuzz).

### 2. Binding

[binder.cc](../src/binder/binder.cc) resolves every name against the catalog and gives every
expression a type by the rules of [ADR 0004](design/0004-decimal-and-type-rules.md). Q3's
`l_extendedprice * (1 - l_discount)` is `DECIMAL(15,2) * DECIMAL(16,2)`, which the rules narrow to
`DECIMAL(18,4)`; the `sum` of it is `DECIMAL(38,4)`, as the plan below shows. The output is a logical plan ([logical.h](../src/plan/logical.h)) that is correct and
unoptimized: a `CrossProduct` of the three tables, a `Filter` holding the whole `WHERE` clause, then
`Aggregate`, `Project`, `Sort` and `Limit`. Errors carry the position of the offending token (`binder
error at <line>:<column>: ...`). Tests: [binder_test.cc](../tests/unit/binder/binder_test.cc),
[tests/sql/binder.test](../tests/sql/binder.test).

### 3. Optimizing

[rules.cc](../src/planner/rules.cc) rewrites the plan. Each rule except join construction has a name
and can be switched off with `--disable-rule` ([ADR 0005](design/0005-optimizer.md)); the order is:

1. `fold_constants`: a subtree with no column in it is evaluated once.
2. `push_filters`: each conjunct that uses one table moves into that table's scan, so
   `c_mktsegment = 'BUILDING'` ends up in the `customer` scan and the two date comparisons in the
   `orders` and `lineitem` scans.
3. Join construction (always on) and `order_joins`: the equalities `c_custkey = o_custkey` and
   `l_orderkey = o_orderkey` are the edges of a join graph ([join_graph.cc](../src/planner/join_graph.cc)),
   and the `CrossProduct` becomes a left-deep tree of hash joins. The order is greedy: at each step
   add the relation that gives the smallest estimated result, using row counts and the min/max in the
   footers plus fixed selectivities ([cardinality.cc](../src/planner/cardinality.cc)).
4. `pick_build_side`: the smaller input of each join becomes the build side.
5. `prune_columns`: each scan keeps only the columns something above it uses; Q3 reads four of
   `lineitem`'s columns and not sixteen.

A comma join that no equality connects is a planner error, not a cross product. Tests:
[rules_test.cc](../tests/unit/planner/rules_test.cc), [join_order_test.cc](../tests/unit/planner/join_order_test.cc),
[cardinality_test.cc](../tests/unit/planner/cardinality_test.cc).

### 4. The physical plan

[physical_planner.cc](../src/planner/physical_planner.cc) turns the logical tree into descriptors
([physical.h](../src/plan/physical.h)), numbering them in pre-order. A `Limit` over a `Sort` becomes one
`TopN`. For each scan it lists the conjuncts of the form `column op literal` that a row group's
min/max can refute (`prune_row_groups`, also switchable). Q3 comes out as follows (printed by
`--explain`, which also prints a row estimate on every line; they are left out here, and are in the
golden file [tests/planner/plans/q3.plan](../tests/planner/plans/q3.plan), which fails the test when a
change alters the plan):

```
TopN #0 [n=10; keys=#1 DESC NULLS LAST, #2 ASC NULLS LAST]
  Project #1 [#0 AS l_orderkey, #3 AS revenue, #1 AS o_orderdate, #2 AS o_shippriority]
    HashAggregate #2 [groups=[#0, #6, #7]; aggs=[SUM((#1 * (1.00 - CAST(#2 AS DECIMAL(16,2))))) -> DECIMAL(38,4)]]
      HashJoin #3 [probe_keys=#0; build_keys=#0]
        Scan #4 [lineitem; cols=l_orderkey,l_extendedprice,l_discount,l_shipdate; filter=(#3 > DATE '1995-03-15'); prune=(#3 > DATE '1995-03-15')]
        HashJoin #5 [probe_keys=#1; build_keys=#0]
          Scan #6 [orders; cols=o_orderkey,o_custkey,o_orderdate,o_shippriority; filter=(#2 < DATE '1995-03-15'); prune=(#2 < DATE '1995-03-15')]
          Scan #7 [customer; cols=c_custkey,c_mktsegment; filter=(#1 = 'BUILDING'); prune=(#1 = 'BUILDING')]
```

The last child of a join is its build side: `customer` is built into a table that `orders` probes,
and the result is built into a table that `lineitem` probes. Tests:
[physical_planner_test.cc](../tests/unit/planner/physical_planner_test.cc),
[plan_golden_test.cc](../tests/planner/plan_golden_test.cc).

### 5. Building and running the operators

`BuildOperators` ([build_operators.cc](../src/exec/build_operators.cc)) instantiates one operator per
descriptor and wraps each in a profiling decorator that counts rows and batches, times the call and
checks for cancellation and the timeout once per batch. The engine then calls `Next()` on the root until it
returns no batch ([ADR 0002](design/0002-pull-based-batches.md)). Every `Next()` returns a `Batch`: up to
2,048 rows by default, as Arrow arrays, plus an optional selection vector. A filter only narrows the
selection; an operator that needs contiguous data calls `Compact`.

- **Scan** ([scan_op.cc](../src/exec/scan_op.cc)) opens the Parquet files, skips the row groups whose
  min/max refute the prune conjuncts, decodes only the columns the plan lists, one row group at a time,
  and applies the scan's filter. Tests: [scan_test.cc](../tests/unit/exec/scan_test.cc).
- **The filter and every expression** are evaluated by [src/expr](../src/expr) a column at a time over the
  live rows ([eval.h](../src/expr/eval.h)), with three-valued logic and overflow as an error. The tests
  compare the evaluator with a row-at-a-time reference on random expression trees:
  [eval_property_test.cc](../tests/unit/expr/eval_property_test.cc).
- **HashJoin** ([hash_join_op.cc](../src/exec/hash_join_op.cc)) reads the whole build side into a
  [JoinHashTable](../src/exec/join_hash_table.cc), which finds a row by an open-addressing index of
  encoded keys that stores the full 64-bit hash ([key_index.cc](../src/exec/key_index.cc)). The probe side
  is processed a batch at a time: hash the keys, find the matches, gather the output columns. Tests:
  [join_hash_table_test.cc](../tests/unit/exec/join_hash_table_test.cc),
  [operators_test.cc](../tests/unit/exec/operators_test.cc).
- **HashAggregate** ([hash_agg_op.cc](../src/exec/hash_agg_op.cc),
  [agg_hash_table.cc](../src/exec/agg_hash_table.cc)) maps each group key to a state index and updates the
  states a column at a time. `SUM` of a decimal accumulates in 128 bits and fails rather than wraps.
  Tests: [agg_hash_table_test.cc](../tests/unit/exec/agg_hash_table_test.cc).
- **TopN** ([topn_op.cc](../src/exec/topn_op.cc)) keeps the best `n` rows seen so far, so that the
  ten rows of Q3 are found without sorting the groups.

The engine copies each result batch into the default memory pool and builds an Arrow `Table`; the
result and a per-operator profile come back as a `QueryResult`.

### 6. The memory budget and spilling

With `--memory-limit`, the engine creates a [MemoryBudget](../src/exec/memory_budget.h) and a
[SpillDirectory](../src/io/spill_directory.h) for this one query. Operators ask the budget before they
grow; the answer is an error, not a hint. A hash join that no longer fits splits both sides by key hash
into partition files (Arrow IPC streams) and joins the pairs one at a time, splitting again if a pair is
still too large. A hash aggregate writes its partial states out by hash and merges them partition by
partition. A sort that does not fit fails with `resource exhausted`, because the engine has no external
sort ([ADR 0007](design/0007-scope.md)). The directory is owned by an RAII object and is gone when the
query ends, whether it succeeded, failed or was cancelled. Details and the measured margin:
[ADR 0006](design/0006-memory-and-spill.md), [docs/operations.md](operations.md). Tests:
[memory_budget_test.cc](../tests/unit/exec/memory_budget_test.cc),
[spill_test.cc](../tests/unit/exec/spill_test.cc),
[tpch_budget_test.cc](../tests/tpch/tpch_budget_test.cc) (the TPC-H queries again, under a limit far
below what they need).

### 7. The answer, checked

[tests/tpch/tpch_test.cc](../tests/tpch/tpch_test.cc) runs Q3 at scale factor 0.01 and compares every
cell with the answer DuckDB gave ([tests/tpch/answers/q3.json](../tests/tpch/answers/q3.json)); the
[differential test](testing.md#the-differential-test) does the same for random queries on random data.
`quarry query --explain-analyze` runs the query and prints, next to each line of the plan, the rows,
batches, own time and peak state of that operator, which is the data behind
[where-time-goes.md](where-time-goes.md).

## What lives where, by question

| If you want to know... | Read |
| --- | --- |
| what SQL is accepted and how it is typed | [docs/sql.md](sql.md) |
| why batches are pulled | [ADR 0002](design/0002-pull-based-batches.md) |
| how the join order is chosen and when the estimate is wrong | [ADR 0005](design/0005-optimizer.md) |
| what a memory limit does | [ADR 0006](design/0006-memory-and-spill.md), [operations](operations.md) |
| what is deliberately missing | [ADR 0007](design/0007-scope.md) |
| how the suite is built to catch mistakes | [docs/testing.md](testing.md) |
| how the numbers were measured | [docs/benchmarks.md](benchmarks.md) |
