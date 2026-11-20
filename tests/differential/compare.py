"""Result comparison for the differential tests (spec sections 5.4 and 12.3).

A Result is a list of (name, type) columns and a list of row tuples of Python values. quarry's JSON
output and DuckDB's result are both turned into one, then compared by diff().

The rules, which docs/testing.md repeats:

  * Without ORDER BY the rows are compared as multisets; with it, in order.
  * Column names are compared case-sensitively (both engines keep the case that was typed).
  * Type names must agree, with one intentional difference: DuckDB's SUM of an INTEGER or BIGINT
    is HUGEINT and quarry's is DECIMAL(38,0). Those two names are one type here, compared by value.
  * Integers and DECIMALs are compared by exact value (Python's Decimal and int are exact).
  * A DOUBLE is compared exactly, except in the columns the caller names as approximate (AVG), where
    the relative error may be at most 1e-12: DuckDB itself varies by one ulp from one query to the
    next on AVG of a DECIMAL. The generator keeps every other DOUBLE aggregate exactly summable:
    the values are k/1024 with |k| <= 2^30, so sums have no rounding and no order dependence.
  * -0.0 equals 0.0, and NULL equals NULL.
"""

import datetime
import decimal
import json
import re
from dataclasses import dataclass, field

AVG_RELATIVE_TOLERANCE = 1e-12

decimal.getcontext().prec = 80  # exact for every value a DECIMAL(38, s) can hold


@dataclass
class Result:
    columns: list  # [(name, type name)]
    rows: list = field(default_factory=list)  # [tuple of Python values]


def _canonical_type(name):
    return "DECIMAL(38,0)" if name == "HUGEINT" else name


def _from_json_value(value, type_name):
    if value is None:
        return None
    if type_name.startswith("DECIMAL"):
        return decimal.Decimal(value)
    if type_name == "DOUBLE":
        return float(value)
    if type_name == "DATE":
        return datetime.date.fromisoformat(value)
    return value


def parse_quarry(text):
    """Parses the CLI's --format json output."""
    document = json.loads(text, parse_float=float, parse_int=int)
    columns = [(c["name"], c["type"]) for c in document["columns"]]
    types = [t for _, t in columns]
    rows = [tuple(_from_json_value(v, t) for v, t in zip(row, types)) for row in document["rows"]]
    return Result(columns=columns, rows=rows)


def from_duckdb(description, rows):
    """A DuckDB cursor's description and fetched rows as a Result."""
    columns = [(d[0], str(d[1])) for d in description]
    return Result(columns=columns, rows=[tuple(r) for r in rows])


def _same_value(a, b, type_name, approximate):
    if a is None or b is None:
        return a is None and b is None
    if type_name == "DOUBLE":
        if a == b:
            return True
        if approximate:
            return abs(a - b) <= AVG_RELATIVE_TOLERANCE * max(abs(a), abs(b))
        return False
    return a == b


def _sort_key(row, types, skip):
    key = []
    for i, (value, type_name) in enumerate(zip(row, types)):
        if i in skip:
            continue
        if value is None:
            key.append((0, 0))
        elif type_name == "DOUBLE":
            key.append((1, value + 0.0))  # -0.0 + 0.0 is 0.0
        elif isinstance(value, bool):
            key.append((1, int(value)))
        else:
            key.append((1, value))
    return key


def _format_row(row):
    return "(" + ", ".join(repr(v) if not isinstance(v, decimal.Decimal) else str(v) for v in row) + ")"


def diff(expected, actual, ordered, approx=frozenset(), limit=8):
    """The differences between DuckDB's result (expected) and quarry's (actual): empty when they
    agree. `approx` is the set of column indexes compared with the AVG tolerance."""
    problems = []
    if len(expected.columns) != len(actual.columns):
        return [f"column count: duckdb {len(expected.columns)}, quarry {len(actual.columns)}"]
    types = []
    for i, ((en, et), (an, at)) in enumerate(zip(expected.columns, actual.columns)):
        if en != an:
            problems.append(f"column {i} name: duckdb {en!r}, quarry {an!r}")
        if _canonical_type(et) != _canonical_type(at):
            problems.append(f"column {i} ({en}) type: duckdb {et}, quarry {at}")
        types.append(_canonical_type(et))
    if problems:
        return problems

    left, right = list(expected.rows), list(actual.rows)
    if len(left) != len(right):
        problems.append(f"row count: duckdb {len(left)}, quarry {len(right)}")
    if not ordered:
        left.sort(key=lambda r: _sort_key(r, types, approx))
        right.sort(key=lambda r: _sort_key(r, types, approx))
    shown = 0
    mismatching = 0
    for index, (l, r) in enumerate(zip(left, right)):
        bad = [
            i
            for i in range(len(types))
            if not _same_value(l[i], r[i], types[i], i in approx)
        ]
        if not bad:
            continue
        mismatching += 1
        if shown < limit:
            names = ", ".join(expected.columns[i][0] for i in bad)
            problems.append(f"row {index} differs in {names}:\n    duckdb: {_format_row(l)}\n    quarry: {_format_row(r)}")
            shown += 1
    if mismatching > shown:
        problems.append(f"... and {mismatching - shown} more differing rows")
    if len(left) != len(right):
        longer, who = (left, "duckdb") if len(left) > len(right) else (right, "quarry")
        extra = longer[min(len(left), len(right)) :]
        for row in extra[:limit]:
            problems.append(f"  only in {who} (after sorting): {_format_row(row)}")
    return problems


_OVERFLOW = re.compile(r"overflow|out of range", re.IGNORECASE)


def is_overflow_error(message):
    """Whether an engine's error message is an arithmetic overflow: quarry says 'decimal overflow'
    or 'integer overflow', DuckDB 'Out of Range Error: Overflow in ...'."""
    return _OVERFLOW.search(message) is not None
