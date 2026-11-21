# ADR 0005: The optimizer

Status: accepted.

## Context

The binder emits a plan that is correct and slow: every table is read in full, the FROM list is a
`CrossProduct`, and the whole WHERE clause sits in one `Filter` above it. Four things make a query
over Parquet fast, and each can be switched off on its own so that its effect can be measured
(`--disable-rule`): constant folding, moving filters to the scan, choosing the join order and the
build side, and reading only the columns and row groups a query needs.

## Decision

### Rules and their order

`Optimize` applies these in order; a disabled rule is skipped.

1. `fold_constants`: a subtree that uses no column is evaluated by `Evaluate` and replaced by the
   literal. The type of the literal is the type the binder gave the subtree (the fold evaluates, it
   never recomputes types), and a subtree that fails to evaluate (an overflow) is left alone, so the
   error stays a run-time one.
2. `push_filters`: each conjunct of a `Filter` that uses columns of one input moves into that
   input: a scan's `filters`, through inner joins and cross products to the scan below. A conjunct
   that uses two inputs stays in the `Filter`.
3. Join construction, always on. A `Filter` over a `CrossProduct` (or a bare `CrossProduct`)
   becomes a left-deep tree of `LogicalJoin`s. The conjuncts of the form `x = y`, where x uses
   columns of one relation and y of another, are the edges of a join graph; each becomes a key pair
   of the join that brings in the later of its two relations, including an edge that closes a cycle
   (Q5's `c_nationkey = s_nationkey`). Everything else stays in a `Filter` above the tree. With
   `push_filters` off that is every single-table conjunct too, which is what makes the ablation
   measure something.
4. `order_joins`: the order of that tree. Greedy: start from the relation with the smallest
   estimate, then repeatedly add the connected relation that gives the smallest estimated result
   (ties go to the earlier relation in FROM). Disabled: FROM order, where a relation that is not
   yet connected waits for the first later one that is.
5. `pick_build_side`: the right input of a join is the build side; the rule swaps the inputs when
   the left estimate is strictly smaller. Disabled: the right input as written builds.
6. `prune_columns`: each scan keeps the columns its ancestors and its own filters use. A scan that
   would keep none keeps its first column, so that rows can still be counted.

`prune_row_groups` is read by `PlanPhysical`: it copies each scan conjunct of the form
`column op literal` (op one of `= < <= > >=`, a flipped comparison turned around) or
`column BETWEEN literal AND literal` into `PhysicalScan::prune_conjuncts`, which the scan checks
against a row group's min/max. The predicate itself is always applied as well; pruning only skips
work.

A comma join with a relation that no chain of equalities connects to the first is a planning error,
`planner error at <line>:<column>: requires a cross product between <a> and <b>`: the engine does not build a
cross product. The position is where `<b>`, the relation that cannot be reached, is written in the query: each
scan of the logical plan carries the position of its FROM item.

Passes that change a node's column layout (join order, build side, column pruning) return a map from
old to new output index, and the parent redirects its expressions through it, so that an expression
never has to be rebuilt from the SQL.

### Cardinality estimates

Fixed constants, not statistics:

| Predicate | Selectivity |
| --- | --- |
| `=` | 0.1 |
| `<>` | 0.9 |
| `<`, `<=`, `>`, `>=`, `BETWEEN` | 0.3 |
| `IN` of n values | min(0.5, 0.1 n) |
| `IS NULL`, `IS NOT NULL` | 0.1, 0.9 |
| `a OR b` | s1 + s2 - s1 s2 |
| `a AND b`, separate scan filters | s1 s2 |
| `NOT a` | 1 - s |
| anything else | 1 |

A scan produces `row_count` times the product of its filters' selectivities. A join of `L` and `R`
on key pairs produces `|L| |R| / D` for the first pair and a tenth of that for each further pair.
The divisor is

    D = max(1, min(B, W))

where `B` is the row count of the smaller of the two base tables the pair's columns come from, and
`W` is the larger of the two columns' domain bounds. A column's domain bound is `max - min + 1` over
the row-group min/max statistics of the scan it traces to, for an integer column. It is infinite
when the column is not an integer, a row group has no statistics for it, or it does not trace to a
scan. The base table and the column are found by following the key through filters, sorts, joins
and plain column projections; when it does not trace back to a scan, the side's own estimate stands
in for `B`. A residual multiplies by its selectivity. A global aggregate produces one row, a grouped
one a tenth of its input (at least one); that figure is only printed, never compared.

### Why a PK-FK formula

The usual formula divides by the larger of the two distinct counts, and the engine has no distinct
counts. For a key join between a table and the table that references it, the number of matches
equals the size of the referencing side, which is `|L| |R| / (rows of the table holding the key)`.
Dividing by the smaller base table is that. It needs only row counts, which the Parquet footer
gives for free.

The PK-FK assumption fails when both columns are foreign keys to a third table. Q5's
`c_nationkey = s_nationkey` is that: both columns hold the 25 nation keys, so every customer
matches about a 25th of the suppliers. At SF1 the formula divided by
<!-- n:ea.q5before.10.est -->10,000<!-- /n --> (the supplier table) and estimated <!-- n:ea.q5before.7.est -->15,000<!-- /n --> rows where the join produces
<!-- n:ea.q5before.7.rows -->12,093,991<!-- /n -->, and the greedy order built a hash table of <!-- n:ea.q5before.5.rows -->18,435,748<!-- /n --> rows (the profile
before the fix, `bench/results/profile/before-join-estimate-fix/explain-analyze-q5.txt`). A column cannot hold more distinct values than its range, and the range
is in the footer, so `W` caps the divisor: for those two columns it is 25. For a real primary-key
pair the range is at least the table size and the divisor is unchanged (`l_orderkey = o_orderkey`:
1.5M orders over a 6M range, divisor 1.5M). The spec allows row counts and min/max, so this adds
no histogram and no sampling.

### Known failure cases

- A many-to-many join on a column with a wide range is still estimated as PK-FK: the cap catches
  only a range smaller than the smaller table. Sparse keys (a range far wider than the number of
  values) and a filter that narrows the range without moving the min and max both defeat it.
- Statistics are per row group over the whole table; a filter on the key column does not narrow
  the domain.
- Correlated predicates: selectivities multiply as if independent.
- A filter on the key column of the smaller table does not reduce the divisor, so the estimate
  after filtering one side can be off by the filter's selectivity either way.
- The constants are guesses: a range predicate keeps 30% <!-- const: the fixed selectivity in src/planner/cardinality.cc --> on a column where it keeps 1% or 99%. The
  ranking among tables still comes out right in TPC-H because the filtered tables are the small ones,
  but the estimates are not close; `est=` in EXPLAIN is a ranking tool, not a prediction.
- Greedy is not optimal: it never reconsiders the first relation, and it ignores that a join may
  make a later one cheaper.
- A non-equality predicate between two tables is applied above the whole join tree, not at the first
  join that has both tables.

### Alternatives

- Dynamic programming over join subsets would find the best left-deep order for up to a dozen
  relations. With six tables and constants for statistics it would pick the same order as the greedy
  one for the benchmark queries, at a cost in code that the estimates do not justify.
- Collecting distinct counts would give the usual formula, at the cost of reading data when a table
  is registered, which the catalog avoids.

### The deliberate flaw

`QUARRY_FLAW=join_order_drops_predicate` makes `order_joins` keep only the first key pair when a
relation is reachable by two. Q5's last join (lineitem, reachable through both `l_orderkey` and
`l_suppkey`) then loses one equality and returns too many rows. `JoinOrderTest` fails under it.
