# ADR 0001: Vectorized interpretation

Status: accepted.

## Context

An expression such as `l_extendedprice * (1 - l_discount) * (1 + l_tax)` has to run once per row
of a table that may have millions of them. There are three ways to execute it.

- **Tuple at a time.** An interpreter walks the expression tree for each row. Every node of the
  tree is a call through a pointer or a `switch` on the node kind, per row, and the values travel as
  tagged unions. The cost is paid per row and per node: for a tree of five nodes and a million rows,
  five million dispatches, each of which defeats the branch predictor's and the cache's usual
  assumptions because the next row runs different code than this node.
- **Just in time.** The expression is compiled to machine code once per query. The loop has no
  dispatch at all and can keep intermediates in registers, so it is the fastest of the three on a
  long scan. The cost is a compiler in the dependency list (LLVM, or hand-written code generation
  per target), compile latency that is paid even by a query that touches a hundred rows, a harder
  debugging story (a miscompiled loop is not a stack trace), and a second implementation of every
  operator to keep equal to the first.
- **Vectorized interpretation.** The interpreter walks the tree once per *batch* of rows. Each node
  is a tight loop over a column of 2,048 values, written once per operator and physical type, with
  the operator and the type fixed at compile time. The dispatch happens once per node per batch, so
  its cost is divided by the batch size, and the loops are the kind that compilers unroll and
  vectorize. The cost is that intermediates are materialised: each node writes a whole column that
  the next node reads back, so the traffic to the cache is higher than in a fused loop, and
  expressions with many small nodes pay for it.

## Decision

quarry evaluates expressions by vectorized interpretation, in the shape of `src/expr/eval*.cc`:

- one templated loop per operator and physical type (`CheckedLoop<int64_t, kMul>`,
  `CompareFixed<Int128>`, ...), selected by a `switch` outside the loop;
- NULLs travel as Arrow validity bitmaps; a kernel combines the operands' bitmaps first and then
  visits only valid rows, so a NULL operand cannot raise an overflow;
- a batch carries an optional selection vector, and the evaluator gathers the live rows of a column
  once, at the leaf, so no kernel knows about selection vectors and every intermediate has one slot
  per live row.
- `AND` and `OR` evaluate both operands for every live row; they do not skip the right side where the
  left decides the answer. The right side's errors are therefore raised even where SQL's three-valued
  logic would not need its value: `FALSE AND l_orderkey * 9223372036854775807 > 0` is an integer
  overflow error, as is the same test under a filter that would drop the row. DuckDB may evaluate in
  a different order, which is why the differential test keeps overflow that depends on evaluation
  order out of the comparison (docs/testing.md).

The reasons, in order of weight:

1. **A tree-walking interpreter is the smallest thing that can be proved correct.** The vectorized
   evaluator has a row-at-a-time reference (`tests/support/reference_eval.cc`) that shares no code
   with it, and a property test (`EvalPropertyTest.VectorizedEqualsReference`) compares the two on
   random batches and random expressions. A JIT would need the same reference and would add a
   second, generated implementation to compare against it.
2. **No new dependency.** The project's boundary (ADR 0003) is Arrow's memory format and the
   Parquet reader. A code generator is a large dependency, and quarry already declines
   the one Arrow offers for this job (its compute layer).
3. **The costs of the alternative that matter are measurable here.** The interpretation overhead
   per batch is a function call and a switch; the materialisation cost shows up as memory traffic
   per node. The benchmark task measures where the time goes (`docs/where-time-goes.md`), so the
   choice is checked against the numbers instead of assumed.

## Consequences

- Fusing is possible later without changing the interface: a kernel for `a * (1 - b)` in one pass
  would replace two nodes, and the property test would keep both honest.
- A batch size is a tuning parameter, 2,048 by default: large enough to amortise a dispatch, small
  enough that a few intermediates stay in the L1 and L2 caches.
- Literals are broadcast into columns, which is wasteful for `x * 2`; a scalar-operand kernel is
  the first optimisation to try if the profile asks for it.
