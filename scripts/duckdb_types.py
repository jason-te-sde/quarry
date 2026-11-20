#!/usr/bin/env python3
"""Pins DuckDB's numeric type rules and writes them into docs/sql.md.

Run from the repository root with `uv run scripts/duckdb_types.py`. Each row is a SELECT typeof(...)
(or a query whose result is shown) executed on the DuckDB version pinned in uv.lock. The table is
written between the `<!-- duckdb-types -->` markers of docs/sql.md, with the DuckDB version. The
unit test TypeRulesTest asserts the same rows, so a DuckDB upgrade that changes a rule fails it.

Pass --check to compare the file with a fresh run instead of writing it.
"""

import argparse
import sys
from pathlib import Path

import duckdb

ROOT = Path(__file__).resolve().parent.parent
DOC = ROOT / "docs" / "sql.md"
BEGIN = "<!-- duckdb-types -->"
END = "<!-- /duckdb-types -->"

# (label, SQL expression for typeof). Aggregates run over a one-row table so they are valid.
TYPEOF_ROWS = [
    ("0.05", "0.05"),
    ("1", "1"),
    ("1 - 0.05", "1 - 0.05"),
    ("DECIMAL(15,2) * DECIMAL(15,2)", "a15_2 * a15_2"),
    ("DECIMAL(31,4) * DECIMAL(16,2)", "a31_4 * a16_2"),
    ("DECIMAL(15,2) + INTEGER", "a15_2 + i"),
    ("DECIMAL(15,2) + DECIMAL(10,4)", "a15_2 + a10_4"),
    ("DECIMAL(15,2) - DECIMAL(10,4)", "a15_2 - a10_4"),
    ("DECIMAL(38,0) * DECIMAL(38,0)", "a38_0 * a38_0"),
    ("DECIMAL(12,5) * DECIMAL(12,5)", "a12_5 * a12_5"),
    ("DECIMAL(18,2) * DECIMAL(19,2)", "a18_2 * a19_2"),
    ("DECIMAL(18,18) * DECIMAL(18,18)", "a18_18 * a18_18"),
    ("DECIMAL(38,10) * DECIMAL(38,10)", "a38_10 * a38_10"),
    ("DECIMAL(38,2) + DECIMAL(38,2)", "a38_2 + a38_2"),
    ("DECIMAL(15,2) * INTEGER", "a15_2 * i"),
    ("DECIMAL(15,2) + BIGINT", "a15_2 + b"),
    ("SUM(DECIMAL(15,2))", "SUM(a15_2)"),
    ("SUM(INTEGER)", "SUM(i)"),
    ("SUM(BIGINT)", "SUM(b)"),
    ("AVG(DECIMAL(15,2))", "AVG(a15_2)"),
    ("AVG(INTEGER)", "AVG(i)"),
    ("MIN(DECIMAL(15,2))", "MIN(a15_2)"),
    ("MAX(DECIMAL(15,2))", "MAX(a15_2)"),
    ("COUNT(*)", "COUNT(*)"),
    ("DATE + INTERVAL 1 MONTH", "d + INTERVAL 1 MONTH"),
    ("INTEGER + BIGINT", "i + b"),
    ("INTEGER * INTEGER", "i * i"),
    ("INTEGER + DOUBLE", "i + x"),
    ("DECIMAL(15,2) + DOUBLE", "a15_2 + x"),
    ("DECIMAL(15,2) = INTEGER", "a15_2 = i"),
]

SETUP = """
CREATE TABLE t AS SELECT
  1::INTEGER AS i, 1::BIGINT AS b, 1.0::DOUBLE AS x, DATE '1994-01-31' AS d,
  1::DECIMAL(15,2) AS a15_2, 1::DECIMAL(10,4) AS a10_4, 1::DECIMAL(16,2) AS a16_2,
  1::DECIMAL(31,4) AS a31_4, 1::DECIMAL(38,0) AS a38_0, 1::DECIMAL(12,5) AS a12_5,
  1::DECIMAL(18,2) AS a18_2, 1::DECIMAL(19,2) AS a19_2, 0.5::DECIMAL(18,18) AS a18_18,
  1::DECIMAL(38,10) AS a38_10, 1::DECIMAL(38,2) AS a38_2
"""


def single(con, sql):
    return con.execute(sql).fetchall()


def build_table(con):
    rows = []
    for label, expr in TYPEOF_ROWS:
        (value,) = single(con, f"SELECT typeof({expr}) FROM t")[0]
        rows.append((label, value))
    return rows


def other_facts(con):
    facts = []
    asc = [r[0] for r in single(con, "SELECT x FROM (VALUES (1),(NULL)) t(x) ORDER BY x")]
    desc = [r[0] for r in single(con, "SELECT x FROM (VALUES (1),(NULL)) t(x) ORDER BY x DESC")]
    facts.append(("ORDER BY x over (1, NULL)", "NULLS LAST" if asc[-1] is None else "NULLS FIRST"))
    facts.append(
        ("ORDER BY x DESC over (1, NULL)", "NULLS FIRST" if desc[0] is None else "NULLS LAST")
    )
    for literal in ("1.005", "-1.005", "1.015", "2.5", "-2.5"):
        scale = 0 if literal in ("2.5", "-2.5") else 2
        precision = 4 if scale else 2
        (value,) = single(con, f"SELECT CAST({literal} AS DECIMAL({precision},{scale}))")[0]
        facts.append((f"CAST({literal} AS DECIMAL({precision},{scale}))", str(value)))
    (value,) = single(con, "SELECT DATE '1994-01-31' + INTERVAL 1 MONTH")[0]
    facts.append(("DATE '1994-01-31' + INTERVAL 1 MONTH", str(value)))
    (value,) = single(con, "SELECT DATE '1998-12-01' - INTERVAL 90 DAY")[0]
    facts.append(("DATE '1998-12-01' - INTERVAL 90 DAY", str(value)))
    return facts


def render(version, rows, facts):
    lines = [BEGIN, f"DuckDB version: {version}", "", "| Expression | typeof |", "| --- | --- |"]
    lines += [f"| `{label}` | `{value}` |" for label, value in rows]
    lines += ["", "| Probe | Result |", "| --- | --- |"]
    lines += [f"| `{label}` | `{value}` |" for label, value in facts]
    lines.append(END)
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    con = duckdb.connect()
    con.execute(SETUP)
    block = render(duckdb.__version__, build_table(con), other_facts(con))

    text = DOC.read_text() if DOC.exists() else ""
    if BEGIN in text and END in text:
        head, rest = text.split(BEGIN, 1)
        _, tail = rest.split(END, 1)
        updated = head + block + tail
    else:
        updated = text + ("\n" if text and not text.endswith("\n") else "") + block + "\n"
    if args.check:
        if updated != text:
            print("docs/sql.md is out of date: run uv run scripts/duckdb_types.py", file=sys.stderr)
            return 1
        return 0
    DOC.parent.mkdir(parents=True, exist_ok=True)
    DOC.write_text(updated)
    print(block)
    return 0


if __name__ == "__main__":
    sys.exit(main())
