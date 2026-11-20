"""The generator measures itself: over seeds 0..299 every grammar feature must appear at least
MINIMUM times, or this test fails and names the starved features. It keeps the generator from
quietly degrading into one that only produces simple queries (spec section 5.4).

Run: uv run python -m unittest discover -s tests/differential -p 'test_generator_coverage.py'
"""

import unittest

import gen_data
import gen_query

SEEDS = range(300)
MINIMUM = 5


def feature_counts(seeds=SEEDS):
    """How many times each feature occurs over the seeds: a Counter."""
    import collections

    counts = collections.Counter()
    for seed in seeds:
        dataset = gen_data.generate(seed)
        counts.update(dataset.features())
        for query in gen_query.generate_queries(seed, dataset):
            counts.update(query.features)
    return counts


class CoverageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.counts = feature_counts()

    def test_every_expected_feature_is_generated_often_enough(self):
        starved = {
            feature: self.counts[feature]
            for feature in gen_query.all_features()
            if self.counts[feature] < MINIMUM
        }
        self.assertEqual(starved, {}, f"features generated fewer than {MINIMUM} times: {starved}")

    def test_the_feature_list_names_the_spec_features(self):
        names = set(gen_query.all_features())
        for required in [
            "pred:eq", "pred:ne", "pred:lt", "pred:le", "pred:gt", "pred:ge",
            "pred:is_null", "pred:is_not_null", "pred:between", "pred:not_between",
            "pred:in", "pred:not_in", "pred:and", "pred:or", "pred:not",
            "agg:sum", "agg:avg", "agg:count", "agg:count_star", "agg:min", "agg:max",
            "join:1", "join:2", "join:3", "join:4", "having", "order_by", "limit",
            "group_by:1", "group_by:2", "group_by:3",
            "data:empty_table", "data:single_row_table", "data:sorted_small_row_groups",
            "data:decimal38_near_1e37", "pred:cross_scale_decimal", "pred:wide_decimal",
            "engine:batch:1", "engine:batch:7", "engine:batch:2048",
            "engine:memory:unlimited", "engine:memory:64KiB", "engine:rule:none",
        ]:
            self.assertIn(required, names)
        for rule in gen_query.RULES:
            self.assertIn(f"engine:rule:{rule}", names)


if __name__ == "__main__":
    unittest.main()
