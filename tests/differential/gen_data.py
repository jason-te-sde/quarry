"""Seeded test data for the differential tests (spec section 5.4).

generate(seed) returns a Dataset of 2 to 4 tables t0..t3, each with the same 19 columns (the
decimal columns get a scale per table, so that joins and comparisons cross scales):

  keys (small domains, so that duplicates occur and joins match):
    k1 BIGINT, k2 INTEGER, ks VARCHAR, kd DATE, kc DECIMAL(12, s)
  small values ("safe": products of two of them cannot overflow):
    si INTEGER, sb BIGINT, sd DECIMAL(12, s), sm DECIMAL(9, s), sf DOUBLE, sdt DATE
  edge values (a few rows hold the type's extremes; arithmetic on them may overflow, in both engines):
    ei INTEGER, eb BIGINT, ed DECIMAL(18, s), ev DECIMAL(25, s), ew DECIMAL(38, s) near +-1e37,
    xf DOUBLE (k/1024, |k| <= 2^30), xdt DATE (0001-01-01 and 9999-12-31 occur)
  others:
    vb BOOLEAN, vs VARCHAR

Every column has 0 to 30% NULLs. A table has 0 to 2,000 rows; every 20 seeds one table is empty and
one holds a single row. One table in four is sorted on a column and written with row groups of 50,
so that row-group pruning has something to prune. DOUBLE values are k/1024 (exact binary
fractions), never NaN or infinity, so sums of them have no rounding. At most three rows of a column
hold extremes, so that the sum of a column cannot overflow in one engine and not in the other
depending on the order of the additions.

Values come from random.Random(f"quarry-data-{seed}"): the same seed gives the same data.
"""

import datetime
import decimal
import random
from dataclasses import dataclass, field
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

decimal.getcontext().prec = 80  # a DECIMAL(38, s) value must not be rounded on its way to Parquet

MAX_ROWS = 2000
SMALL_ROW_GROUP = 50
INT32_MAX = 2**31 - 1
INT64_MAX = 2**63 - 1
BASE_DATE = datetime.date(2020, 1, 1)


@dataclass
class Column:
    name: str
    kind: str  # int, dec, dbl, date, str, bool
    sql_type: str
    arrow_type: object
    precision: int = 0
    scale: int = 0
    role: str = "value"  # key, small, edge, value
    family: str = ""  # join-key family: int, str, date, dec
    values: list = field(default_factory=list)


@dataclass
class Table:
    name: str
    columns: list
    rows: int
    row_group_size: int | None = None
    sorted_by: str | None = None
    has_extreme_wide: bool = False

    def column(self, name):
        return next(c for c in self.columns if c.name == name)


@dataclass
class Dataset:
    seed: int
    tables: list

    def features(self):
        out = []
        for table in self.tables:
            if table.rows == 0:
                out.append("data:empty_table")
            if table.rows == 1:
                out.append("data:single_row_table")
            if table.sorted_by is not None:
                out.append("data:sorted_small_row_groups")
            if table.has_extreme_wide:
                out.append("data:decimal38_near_1e37")
            if any(v is None for c in table.columns for v in c.values):
                out.append("data:nulls")
        return out

    def write(self, directory):
        """Writes tN.parquet for every table into the directory (created if needed)."""
        directory = Path(directory)
        directory.mkdir(parents=True, exist_ok=True)
        for table in self.tables:
            arrays = [pa.array(c.values, type=c.arrow_type) for c in table.columns]
            names = [c.name for c in table.columns]
            data = pa.Table.from_arrays(arrays, names=names)
            pq.write_table(
                data,
                (directory / f"{table.name}.parquet").as_posix(),
                row_group_size=table.row_group_size,
                compression="snappy",
                write_statistics=True,
            )


def _dec(raw, scale):
    return decimal.Decimal(raw).scaleb(-scale)


def _row_count(rng):
    band = rng.random()
    if band < 0.15:
        return rng.randint(2, 20)
    if band < 0.40:
        return rng.randint(20, 150)
    if band < 0.70:
        return rng.randint(150, 600)
    if band < 0.90:
        return rng.randint(600, 1200)
    return rng.randint(1200, MAX_ROWS)


def _with_nulls(rng, values):
    fraction = 0.0 if rng.random() < 0.3 else rng.uniform(0.0, 0.3)
    return [None if rng.random() < fraction else v for v in values]


def _spread_extremes(rng, values, extremes):
    """Overwrites up to three random positions with the extreme values."""
    if not values:
        return
    for extreme in rng.sample(extremes, k=min(len(extremes), rng.randint(0, 3))):
        values[rng.randrange(len(values))] = extreme


def _domain(rng, n):
    return max(1, int(n * rng.uniform(0.2, 1.2)))


def _key_indexes(rng, n):
    """n key numbers drawn from a small domain; one table column in ten is skewed to a hot key."""
    domain = _domain(rng, n)
    skewed = rng.random() < 0.1
    return [0 if skewed and rng.random() < 0.4 else rng.randrange(domain) for _ in range(n)]


WORDS = ["", "a", "A", "b", "ab", "abc", "b'c", "é", "zz", " ", "Z", "10", "9", "a b"]


def _string_value(rng):
    return rng.choice(WORDS) if rng.random() < 0.6 else f"w{rng.randrange(40)}"


def _double(k):
    return k / 1024


def _build_table(rng, name, n):
    scales = {
        "kc": rng.choice([0, 2, 4]),
        "sd": rng.choice([1, 2, 3]),
        "sm": rng.choice([0, 4]),
        "ed": rng.choice([0, 2, 6]),
        "ev": rng.choice([0, 3, 9]),
        "ew": rng.choice([0, 2, 6, 10]),
    }
    columns = []

    def add(name_, kind, sql_type, arrow_type, values, role, family="", precision=0, scale=0):
        columns.append(
            Column(
                name_, kind, sql_type, arrow_type, precision, scale, role, family, _with_nulls(rng, values)
            )
        )

    def decimal_column(name_, precision, role, values_raw):
        s = scales[name_]
        add(
            name_,
            "dec",
            f"DECIMAL({precision},{s})",
            pa.decimal128(precision, s),
            [_dec(r, s) for r in values_raw],
            role,
            family="dec" if role == "key" else "",
            precision=precision,
            scale=s,
        )

    add("k1", "int", "BIGINT", pa.int64(), _key_indexes(rng, n), "key", "int")
    add("k2", "int", "INTEGER", pa.int32(), _key_indexes(rng, n), "key", "int")
    add("ks", "str", "VARCHAR", pa.string(), [f"s{i}" for i in _key_indexes(rng, n)], "key", "str")
    add(
        "kd",
        "date",
        "DATE",
        pa.date32(),
        [BASE_DATE + datetime.timedelta(days=i) for i in _key_indexes(rng, n)],
        "key",
        "date",
    )
    half = 5 * 10 ** max(scales["kc"] - 1, 0) if scales["kc"] else 0
    decimal_column(
        "kc",
        12,
        "key",
        [i * 10 ** scales["kc"] + (half if rng.random() < 0.3 else 0) for i in _key_indexes(rng, n)],
    )

    add("si", "int", "INTEGER", pa.int32(), [rng.randint(-1000, 1000) for _ in range(n)], "small")
    add("sb", "int", "BIGINT", pa.int64(), [rng.randint(-10**6, 10**6) for _ in range(n)], "small")
    decimal_column("sd", 12, "small", [rng.randint(-10**5, 10**5) for _ in range(n)])
    decimal_column("sm", 9, "small", [rng.randint(-10**4, 10**4) for _ in range(n)])
    add(
        "sf",
        "dbl",
        "DOUBLE",
        pa.float64(),
        [_double(rng.randint(-2000, 2000)) for _ in range(n)],
        "small",
    )
    add(
        "sdt",
        "date",
        "DATE",
        pa.date32(),
        [datetime.date(2000, 1, 1) + datetime.timedelta(days=rng.randint(0, 11000)) for _ in range(n)],
        "small",
    )

    ei = [rng.randint(-10**6, 10**6) for _ in range(n)]
    _spread_extremes(rng, ei, [INT32_MAX, -INT32_MAX - 1, 0, 1, -1])
    add("ei", "int", "INTEGER", pa.int32(), ei, "edge")
    eb = [rng.randint(-10**9, 10**9) for _ in range(n)]
    _spread_extremes(rng, eb, [INT64_MAX, -INT64_MAX - 1, 0, INT32_MAX + 1])
    add("eb", "int", "BIGINT", pa.int64(), eb, "edge")
    ed = [rng.randint(-10**6, 10**6) for _ in range(n)]
    _spread_extremes(rng, ed, [10**18 - 1, -(10**18 - 1), 10**17])
    decimal_column("ed", 18, "edge", ed)
    ev = [rng.randint(-10**6, 10**6) for _ in range(n)]
    _spread_extremes(rng, ev, [10**24 - rng.randint(0, 999), -(10**24) + rng.randint(0, 999), 10**20])
    decimal_column("ev", 25, "edge", ev)
    ew = [rng.randint(-10**6, 10**6) for _ in range(n)]
    wide = [10**37 + rng.randint(-1000, 1000), -(10**37) + rng.randint(-1000, 1000), 10**37]
    _spread_extremes(rng, ew, wide)
    decimal_column("ew", 38, "edge", ew)
    xf = [
        _double(rng.randint(-(2**30), 2**30)) if rng.random() < 0.3 else _double(rng.randint(-5000, 5000))
        for _ in range(n)
    ]
    _spread_extremes(rng, xf, [_double(2**30), _double(-(2**30)), -0.0])
    add("xf", "dbl", "DOUBLE", pa.float64(), xf, "edge")
    xdt = [datetime.date(1990, 1, 1) + datetime.timedelta(days=rng.randint(0, 15000)) for _ in range(n)]
    _spread_extremes(rng, xdt, [datetime.date(1, 1, 1), datetime.date(9999, 12, 31)])
    add("xdt", "date", "DATE", pa.date32(), xdt, "edge")

    add("vb", "bool", "BOOLEAN", pa.bool_(), [rng.random() < 0.5 for _ in range(n)], "value")
    add("vs", "str", "VARCHAR", pa.string(), [_string_value(rng) for _ in range(n)], "value")

    table = Table(name, columns, n)
    limit = decimal.Decimal(10) ** (30 - scales["ew"])
    table.has_extreme_wide = any(v is not None and abs(v) > limit for v in table.column("ew").values)
    return table


def _sort_table(rng, table):
    candidates = ["k1", "sb", "ei", "eb", "ed", "sdt", "sd"]
    name = rng.choice(candidates)
    key = table.column(name).values
    order = sorted(range(table.rows), key=lambda i: (key[i] is None, key[i] if key[i] is not None else 0))
    for column in table.columns:
        column.values = [column.values[i] for i in order]
    table.sorted_by = name
    table.row_group_size = SMALL_ROW_GROUP


def generate(seed):
    rng = random.Random(f"quarry-data-{seed}")
    count = rng.randint(2, 4)
    sizes = [_row_count(rng) for _ in range(count)]
    if seed % 20 == 0:
        sizes[0] = 0
    if seed % 20 == 10:
        sizes[0] = 1
    tables = []
    for i, n in enumerate(sizes):
        table = _build_table(rng, f"t{i}", n)
        if rng.random() < 0.25 and n > 0:
            _sort_table(rng, table)
        elif rng.random() < 0.3:
            table.row_group_size = 400
        tables.append(table)
    return Dataset(seed, tables)
