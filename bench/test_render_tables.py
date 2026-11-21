"""Tests of bench/render_tables.py: tables come from committed results, and --check notices a
number that no result produced.

Run from the repository root:  uv run python -m unittest discover -s bench -v
"""

import json
import tempfile
import unittest
from pathlib import Path

import render_tables


def cell(engine, config, query, median, **extra):
    base = {
        "engine": engine, "config": config, "query": query, "dataset": "default", "group": "engines",
        "status": "ok", "median_ms": median, "min_ms": median - 1, "max_ms": median + 1,
        "peak_rss_bytes": 2**20, "idle_rss_bytes": 2**19, "pool_peak_bytes": 2**10,
        "scan_pool_peak_bytes": 2**11, "spilled_bytes": 0, "load_flag": False,
    }
    base.update(extra)
    return base


def main_result(quarry_median):
    cells = []
    for q in render_tables.MAIN_QUERIES:
        cells.append(cell("quarry", "default", q, quarry_median))
        cells.append(cell("duckdb", "threads=1", q, 10.0, threads=1))
        cells.append(cell("duckdb", "threads=default", q, 5.0, threads=8))
    return {
        "suite": "main", "repo_commit": "0123456789abcdef", "timestamp_utc": "2026-01-01T00:00:00Z",
        "protocol": {"load_wait_s": 120}, "cells": cells,
    }


class RenderTablesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "bench" / "results").mkdir(parents=True)
        self.readme = self.root / "README.md"
        self.readme.write_text("# t\n\n<!-- bench:main -->\nstale\n<!-- /bench:main -->\n")

    def result(self, name, document):
        (self.root / "bench" / "results" / name).write_text(json.dumps(document))

    def run_tables(self, *flags):
        return render_tables.main(["--repo-root", str(self.root), *flags])

    def test_a_missing_result_says_so(self):
        self.assertEqual(self.run_tables(), 0)
        self.assertIn("No result is committed", self.readme.read_text())

    def test_renders_the_newest_result_and_the_ratio(self):
        self.result("main-20260101T000000Z.json", main_result(30.0))
        self.result("main-20260102T000000Z.json", main_result(25.0))
        self.assertEqual(self.run_tables(), 0)
        text = self.readme.read_text()
        self.assertIn("| Q1 | 25.0 (24.0-26.0) | 10.0 (9.0-11.0) | 5.0 (4.0-6.0) | 2.50 |", text)
        self.assertNotIn("30.0", text)
        self.assertIn("main-20260102T000000Z.json", text)

    def test_check_passes_after_rendering_and_fails_on_an_edited_number(self):
        self.result("main-20260101T000000Z.json", main_result(25.0))
        self.run_tables()
        self.assertEqual(self.run_tables("--check"), 0)
        self.readme.write_text(self.readme.read_text().replace("25.0 (24.0-26.0)", "9.9 (9.0-11.0)", 1))
        self.assertEqual(self.run_tables("--check"), 1)

    def test_a_result_recorded_without_waiting_for_the_load_is_ignored(self):
        document = main_result(25.0)
        document["protocol"]["load_wait_s"] = 0
        self.result("main-20260101T000000Z.json", document)
        self.run_tables()
        self.assertIn("No result is committed", self.readme.read_text())

    def test_an_unknown_table_is_an_error(self):
        self.readme.write_text("<!-- bench:nope -->\n<!-- /bench:nope -->\n")
        self.assertEqual(self.run_tables(), 1)


if __name__ == "__main__":
    unittest.main()
