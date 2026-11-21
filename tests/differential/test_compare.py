"""Tests of the result comparator (tests/differential/compare.py).

Run: uv run python -m unittest discover -s tests/differential -p 'test_compare.py'
"""

import unittest
from decimal import Decimal

import compare


def quarry(columns, rows):
    """A quarry JSON result, as the CLI prints it, parsed."""
    import json

    return compare.parse_quarry(
        json.dumps({"columns": [{"name": n, "type": t} for n, t in columns], "rows": rows})
    )


def duck(columns, rows):
    return compare.Result(columns=list(columns), rows=[tuple(r) for r in rows])


class OrderTest(unittest.TestCase):
    def test_reordered_rows_match_without_order_by(self):
        expected = duck([("a", "BIGINT")], [(1,), (2,), (3,)])
        actual = quarry([("a", "BIGINT")], [[3], [1], [2]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])

    def test_reordered_rows_differ_with_order_by(self):
        expected = duck([("a", "BIGINT")], [(1,), (2,), (3,)])
        actual = quarry([("a", "BIGINT")], [[3], [1], [2]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=True), [])

    def test_a_multiset_is_not_a_set(self):
        expected = duck([("a", "BIGINT")], [(1,), (1,), (2,)])
        actual = quarry([("a", "BIGINT")], [[1], [2], [2]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_a_missing_row_is_reported(self):
        expected = duck([("a", "BIGINT")], [(1,), (2,)])
        actual = quarry([("a", "BIGINT")], [[1]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_null_equals_null(self):
        expected = duck([("a", "BIGINT")], [(None,), (1,)])
        actual = quarry([("a", "BIGINT")], [[1], [None]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])

    def test_null_is_not_zero(self):
        expected = duck([("a", "BIGINT")], [(None,)])
        actual = quarry([("a", "BIGINT")], [[0]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])


class TypeTest(unittest.TestCase):
    def test_hugeint_matches_decimal_38_0_by_value(self):
        expected = duck([("s", "HUGEINT")], [(5,)])
        actual = quarry([("s", "DECIMAL(38,0)")], [["5"]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])

    def test_hugeint_with_a_different_value_differs(self):
        expected = duck([("s", "HUGEINT")], [(5,)])
        actual = quarry([("s", "DECIMAL(38,0)")], [["6"]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_beyond_int64_is_compared_exactly(self):
        big = 10**37 + 1
        expected = duck([("s", "HUGEINT")], [(big,)])
        actual = quarry([("s", "DECIMAL(38,0)")], [[str(big)]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])
        actual = quarry([("s", "DECIMAL(38,0)")], [[str(big - 1)]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_other_type_names_must_agree(self):
        expected = duck([("a", "BIGINT")], [(1,)])
        actual = quarry([("a", "INTEGER")], [[1]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_decimal_scale_is_part_of_the_type(self):
        expected = duck([("a", "DECIMAL(12,2)")], [(Decimal("1.50"),)])
        actual = quarry([("a", "DECIMAL(12,4)")], [["1.5000"]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_decimal_values_are_exact(self):
        expected = duck([("a", "DECIMAL(38,6)")], [(Decimal("12345678901234567890123456.123457"),)])
        same = quarry([("a", "DECIMAL(38,6)")], [["12345678901234567890123456.123457"]])
        off = quarry([("a", "DECIMAL(38,6)")], [["12345678901234567890123456.123456"]])
        self.assertEqual(compare.diff(expected, same, ordered=False), [])
        self.assertNotEqual(compare.diff(expected, off, ordered=False), [])

    def test_column_names_are_case_sensitive(self):
        expected = duck([("MyName", "BIGINT")], [(1,)])
        actual = quarry([("myname", "BIGINT")], [[1]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_column_count_must_agree(self):
        expected = duck([("a", "BIGINT"), ("b", "BIGINT")], [(1, 2)])
        actual = quarry([("a", "BIGINT")], [[1]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_dates_and_strings_and_booleans(self):
        import datetime

        expected = duck(
            [("d", "DATE"), ("s", "VARCHAR"), ("b", "BOOLEAN")],
            [(datetime.date(2020, 2, 29), "x'y", True)],
        )
        actual = quarry([("d", "DATE"), ("s", "VARCHAR"), ("b", "BOOLEAN")], [["2020-02-29", "x'y", True]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])
        actual = quarry([("d", "DATE"), ("s", "VARCHAR"), ("b", "BOOLEAN")], [["2020-03-01", "x'y", True]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])


class DoubleTest(unittest.TestCase):
    def test_doubles_are_exact_by_default(self):
        expected = duck([("x", "DOUBLE")], [(1.0,)])
        actual = quarry([("x", "DOUBLE")], [[1.0000000000001]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False), [])

    def test_avg_within_relative_1e_12_matches(self):
        expected = duck([("avg", "DOUBLE")], [(1000.0,)])
        actual = quarry([("avg", "DOUBLE")], [[1000.0 * (1 + 5e-13)]])
        self.assertEqual(compare.diff(expected, actual, ordered=False, approx={0}), [])

    def test_avg_off_by_relative_1e_10_differs(self):
        expected = duck([("avg", "DOUBLE")], [(1000.0,)])
        actual = quarry([("avg", "DOUBLE")], [[1000.0 * (1 + 1e-10)]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False, approx={0}), [])

    def test_the_tolerance_is_relative(self):
        expected = duck([("avg", "DOUBLE")], [(1e-9,)])
        actual = quarry([("avg", "DOUBLE")], [[1e-9 * (1 + 1e-10)]])
        self.assertNotEqual(compare.diff(expected, actual, ordered=False, approx={0}), [])

    def test_negative_zero_equals_zero(self):
        expected = duck([("x", "DOUBLE")], [(-0.0,), (1.5,)])
        actual = quarry([("x", "DOUBLE")], [[1.5], [0.0]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])
        expected = duck([("x", "DOUBLE")], [(0.0,)])
        actual = quarry([("x", "DOUBLE")], [[-0.0]])
        self.assertEqual(compare.diff(expected, actual, ordered=True), [])

    def test_a_double_printed_as_an_integer_is_a_double(self):
        expected = duck([("x", "DOUBLE")], [(5.0,)])
        actual = quarry([("x", "DOUBLE")], [[5]])
        self.assertEqual(compare.diff(expected, actual, ordered=False), [])


class ErrorTest(unittest.TestCase):
    def test_overflow_class_errors(self):
        self.assertTrue(compare.is_overflow_error("quarry: decimal overflow in multiplication"))
        self.assertTrue(compare.is_overflow_error("Out of Range Error: Overflow in multiplication of DECIMAL(18)"))
        self.assertTrue(compare.is_overflow_error("Out of Range Error: Overflow in addition of INT32 (2147483647 + 1)!"))
        self.assertFalse(compare.is_overflow_error("binder error at 1:8: unknown column"))
        self.assertFalse(compare.is_overflow_error("Binder Error: Referenced column not found"))


if __name__ == "__main__":
    unittest.main()
