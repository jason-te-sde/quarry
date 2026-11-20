#!/usr/bin/env python3
"""Fills in the expected results of tests/sql/results.test with DuckDB.

Run from the repository root: `uv run scripts/gen_goldens.py`.

A `query` block of the file is the word `query`, the SQL (any number of lines), a line `----` and one
line of expected JSON; blocks are separated by blank lines. This script runs the SQL of every
`query` block on DuckDB over the same Parquet files the engine reads (the TPC-H SF0.01 tables,
events and mixed) and writes the result after the `----` line in the JSON result format of
tools/output_format.cc, which scripts/gen_tpch.py also writes. Everything else in the file is kept
as it is.

--check does not write: it exits with status 1 when the file is not what this script would write.
"""

import argparse
import os
import sys
from pathlib import Path

import duckdb

import gen_tpch

ROOT = Path(__file__).resolve().parent.parent
RESULTS = Path("tests/sql/results.test")
DATA_DIRS = [Path("tests/data/tpch-sf0.01"), Path("tests/data/pruning"), Path("tests/data/nulls")]


def connect():
    con = duckdb.connect()
    for directory in DATA_DIRS:
        for path in sorted(directory.glob("*.parquet")):
            con.execute(f"create view {path.stem} as select * from read_parquet('{path.as_posix()}')")
    return con


def run(con, sql):
    cursor = con.execute(sql)
    names = [d[0] for d in cursor.description]
    types = [str(d[1]) for d in cursor.description]
    return gen_tpch.serialize_result(names, types, cursor.fetchall())


def rewrite(text, con):
    """The file with every query block's expected line regenerated."""
    out = []
    lines = text.split("\n")
    i = 0
    count = 0
    while i < len(lines):
        line = lines[i]
        if line != "query":
            out.append(line)
            i += 1
            continue
        # A query block: header, SQL up to "----", then the old expected line, if any.
        i += 1
        sql_lines = []
        while i < len(lines) and lines[i] != "----" and lines[i] != "":
            sql_lines.append(lines[i])
            i += 1
        if i < len(lines) and lines[i] == "----":
            i += 1
            if i < len(lines) and lines[i] != "":
                i += 1  # the old expected line
        sql = "\n".join(sql_lines)
        try:
            expected = run(con, sql)
        except Exception as error:  # a query DuckDB rejects is a mistake in the test file
            sys.exit(f"DuckDB rejected a query:\n{sql}\n{error}")
        out.extend(["query", *sql_lines, "----", expected])
        count += 1
    return "\n".join(out), count


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="do not write; fail if the file is out of date")
    args = parser.parse_args()
    os.chdir(ROOT)
    text = RESULTS.read_text(encoding="utf-8")
    new_text, count = rewrite(text, connect())
    if args.check:
        if new_text != text:
            sys.exit(f"{RESULTS} is out of date: run uv run scripts/gen_goldens.py")
        print(f"{RESULTS}: {count} queries up to date (DuckDB {duckdb.__version__})")
        return
    RESULTS.write_text(new_text, encoding="utf-8")
    print(f"wrote {count} expected results to {RESULTS} (DuckDB {duckdb.__version__})")


if __name__ == "__main__":
    main()
