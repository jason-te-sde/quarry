"""Tests of the runner's judgement of memory-limit refusals (tests/differential/run.py)."""

import unittest

import run


class RefusalTest(unittest.TestCase):
    def test_a_sort_over_budget_is_by_design(self):
        message = "quarry: resource exhausted: Sort #0 needs 28 bytes, budget 65536, in use 65521"
        self.assertEqual(run.refusal_reason(message), "sort over budget")

    def test_a_join_partition_at_maximum_depth_is_by_design(self):
        message = (
            "quarry: resource exhausted: HashJoin #2 needs 65345 bytes, budget 65536, in use 61879: "
            "a partition does not fit although the input was split by hash down to partition depth 3"
        )
        self.assertEqual(run.refusal_reason(message), "join partition at maximum depth")

    def test_other_refusals_are_not(self):
        for message in [
            "quarry: resource exhausted: HashAggregate #1 needs 100 bytes, budget 65536, in use 65500",
            "quarry: resource exhausted: HashJoin #2 needs 100 bytes, budget 65536, in use 65500",
            "quarry: resource exhausted: TopN #1 needs 100 bytes, budget 65536, in use 65500",
            "quarry: resource exhausted: Sort #0 needs 28 bytes, budget 65536, in use 65521 and more",
            "quarry: out of memory",
        ]:
            self.assertIsNone(run.refusal_reason(message), message)

    def test_an_undocumented_refusal_is_a_mismatch_not_a_rerun(self):
        calls = []
        stats = run.Stats()
        query = _query(memory_limit="64KiB")
        original = (run.run_duckdb, run.run_quarry)
        run.run_duckdb = lambda con, sql: ("ok", _result())
        refusal = "quarry: resource exhausted: HashAggregate #1 needs 1 bytes, budget 65536, in use 65536"

        def fake_quarry(quarry, data_dir, sql, args):
            calls.append(args)
            return ("error", refusal) if "--memory-limit" in args else ("ok", _result())

        run.run_quarry = fake_quarry
        try:
            problems = run.check_query(None, None, None, query, stats, 10**9)
        finally:
            run.run_duckdb, run.run_quarry = original
        self.assertEqual(len(calls), 1, "the query must not be re-run without the limit")
        self.assertTrue(problems and "not by design" in problems[0])
        self.assertEqual(sum(stats.budget_refusals.values()), 0)

    def test_a_documented_refusal_is_rerun_and_compared(self):
        stats = run.Stats()
        query = _query(memory_limit="64KiB")
        original = (run.run_duckdb, run.run_quarry)
        run.run_duckdb = lambda con, sql: ("ok", _result())
        refusal = "quarry: resource exhausted: Sort #0 needs 1 bytes, budget 65536, in use 65536"
        run.run_quarry = lambda q, d, sql, args: ("error", refusal) if "--memory-limit" in args else ("ok", _result())
        try:
            problems = run.check_query(None, None, None, query, stats, 10**9)
        finally:
            run.run_duckdb, run.run_quarry = original
        self.assertEqual(problems, [])
        self.assertEqual(dict(stats.budget_refusals), {"sort over budget": 1})
        self.assertTrue(stats.summary())


def _result():
    import compare

    return compare.Result(columns=[("a", "BIGINT")], rows=[(1,)])


def _query(memory_limit):
    import gen_query

    options = gen_query.Options(batch_size=7, memory_limit=memory_limit, disabled_rule=None)
    return gen_query.Query(0, 0, "SELECT 1", "SELECT 1", False, frozenset(), options, [])


if __name__ == "__main__":
    unittest.main()
