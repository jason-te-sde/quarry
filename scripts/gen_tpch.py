#!/usr/bin/env python3
"""Generates quarry's test and benchmark data with DuckDB, and the answers the tests compare to.

Run from the repository root with `uv run scripts/gen_tpch.py ...`:

  --sf 0.01 --row-group-size 1000 --out tests/data/tpch-sf0.01
      the committed test data; also writes tests/data/pruning/events.parquet and
      tests/tpch/answers/q{1,3,5,6}.json
  --sf 1 --out bench/data/sf1
      the benchmark data, with DuckDB's default row-group size
  --sf 1 --sorted-lineitem --out bench/data/sf1-sorted
      the same, with lineitem ordered by l_shipdate

DuckDB never writes a row group of fewer than 2048 rows, so --row-group-size 1000 yields groups
of 2048 (the smallest it can write). The TPC-H tables come from DuckDB's `tpch` extension (CALL dbgen). The extension is not bundled in
the pip wheel, so `INSTALL tpch` fetches it from DuckDB's official extension repository the first
time the script runs; the version matches the pinned wheel.

The answers are produced by running the committed query files on DuckDB over the generated Parquet
files and serialising the result in the JSON result format shared with tools/output_format.cc:

  {"columns":[{"name":...,"type":"DECIMAL(38,2)"}],"rows":[[...]]}

DECIMAL is a string with exactly `scale` decimals, DOUBLE a JSON number printed with %.17g, DATE
"YYYY-MM-DD", NULL null, BOOLEAN true/false. Each answer file also carries "duckdb_version" as a
last member, which the C++ side does not emit and a comparison ignores.

Every path written is relative to the repository root.
"""

import argparse
import datetime
import decimal
import json
import os
import re
import shutil
import sys
from pathlib import Path

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

ROOT = Path(__file__).resolve().parent.parent
TABLES = ["region", "nation", "supplier", "customer", "part", "partsupp", "orders", "lineitem"]
QUERIES = ["q1", "q3", "q5", "q6"]
EVENTS_PATH = Path("tests/data/pruning/events.parquet")
ANSWERS_DIR = Path("tests/tpch/answers")
QUERIES_DIR = Path("tests/tpch/queries")


def json_value(value, type_name):
    """Serialises one result value; see the module docstring for the format."""
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, decimal.Decimal):
        match = re.fullmatch(r"DECIMAL\((\d+),(\d+)\)", type_name)
        if match is None:
            raise ValueError(f"decimal value in a column of type {type_name}")
        scale = int(match.group(2))
        return json.dumps(f"{value:.{scale}f}")
    if isinstance(value, float):
        if value != value or value in (float("inf"), float("-inf")):
            raise ValueError("NaN and infinity have no JSON form")
        return "%.17g" % value
    if isinstance(value, int):
        return str(value)
    if isinstance(value, datetime.date):
        return json.dumps(value.isoformat())
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    raise ValueError(f"unsupported value {value!r} of type {type_name}")


def serialize_result(names, types, rows):
    columns = ",".join(
        '{"name":%s,"type":%s}' % (json.dumps(n, ensure_ascii=False), json.dumps(t))
        for n, t in zip(names, types)
    )
    body = ",".join("[" + ",".join(json_value(v, t) for v, t in zip(row, types)) + "]" for row in rows)
    return '{"columns":[%s],"rows":[%s]}' % (columns, body)


def write_tables(con, out, row_group_size, sorted_lineitem):
    out.mkdir(parents=True, exist_ok=True)
    for table in TABLES:
        target = out / f"{table}.parquet"
        source = table
        if table == "lineitem" and sorted_lineitem:
            source = "(select * from lineitem order by l_shipdate)"
        options = "FORMAT parquet, COMPRESSION snappy"
        if row_group_size is not None:
            options += f", ROW_GROUP_SIZE {row_group_size}"
        con.execute(f"COPY {source} TO '{target.as_posix()}' ({options})")
        print(f"wrote {target}")


def write_events():
    """1,000 rows, one per day from 2020-01-01, in ten row groups of 100.

    DuckDB's Parquet writer cannot make a row group smaller than its vector size (2048 rows), so
    this one file is written with pyarrow; the pruning tests need row groups with narrow ranges.
    """
    EVENTS_PATH.parent.mkdir(parents=True, exist_ok=True)
    first = datetime.date(2020, 1, 1)
    table = pa.table(
        {
            "event_date": pa.array([first + datetime.timedelta(days=i) for i in range(1000)], pa.date32()),
            "id": pa.array(range(1000), pa.int64()),
            "amount": pa.array(
                [decimal.Decimal(i * 37 % 10000).scaleb(-2) for i in range(1000)], pa.decimal128(12, 2)
            ),
        }
    )
    pq.write_table(table, EVENTS_PATH.as_posix(), row_group_size=100, compression="snappy", write_statistics=True)
    print(f"wrote {EVENTS_PATH}")


def write_answers(table_dir):
    ANSWERS_DIR.mkdir(parents=True, exist_ok=True)
    con = duckdb.connect()
    for table in TABLES:
        path = (table_dir / f"{table}.parquet").as_posix()
        con.execute(f"create view {table} as select * from read_parquet('{path}')")
    for name in QUERIES:
        sql = (QUERIES_DIR / f"{name}.sql").read_text(encoding="utf-8")
        cursor = con.execute(sql)
        names = [d[0] for d in cursor.description]
        types = [str(d[1]) for d in cursor.description]
        rows = cursor.fetchall()
        text = serialize_result(names, types, rows)
        text = text[:-1] + ',"duckdb_version":%s}' % json.dumps(duckdb.__version__)
        target = ANSWERS_DIR / f"{name}.json"
        target.write_text(text + "\n", encoding="utf-8")
        print(f"wrote {target} ({len(rows)} rows)")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--sf", type=float, required=True, help="TPC-H scale factor")
    parser.add_argument("--out", type=Path, required=True, help="output directory, relative to the repository root")
    parser.add_argument("--row-group-size", type=int, default=None)
    parser.add_argument("--sorted-lineitem", action="store_true", help="write lineitem ordered by l_shipdate")
    parser.add_argument(
        "--no-fixtures",
        action="store_true",
        help="do not write events.parquet and the answers (written by default for --sf <= 0.1)",
    )
    args = parser.parse_args()
    if args.out.is_absolute():
        sys.exit("--out must be relative to the repository root")

    os.chdir(ROOT)
    if args.out.exists():
        shutil.rmtree(args.out)

    con = duckdb.connect()
    con.execute("INSTALL tpch")
    con.execute("LOAD tpch")
    con.execute(f"CALL dbgen(sf={args.sf})")
    write_tables(con, args.out, args.row_group_size, args.sorted_lineitem)
    if args.sf <= 0.1 and not args.no_fixtures:
        write_events()
        write_answers(args.out)


if __name__ == "__main__":
    main()
