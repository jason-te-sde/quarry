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

    def test_a_crashed_cell_is_shown_like_a_timeout(self):
        document = main_result(25.0)
        document["cells"][0] = {
            "engine": "quarry", "config": "default", "query": "q1", "dataset": "default", "group": "engines",
            "status": "crashed", "error": "died", "load_flag": None,
        }
        self.result("main-20260101T000000Z.json", document)
        self.assertEqual(self.run_tables(), 0)
        self.assertIn("| Q1 | crashed |", self.readme.read_text())

    def test_the_load_note_lists_only_the_cells_of_its_own_table_and_names_the_dataset(self):
        def flagged_cell(group, dataset, query, flag):
            return cell("quarry", "default", query, 10.0, group=group, dataset=dataset, load_flag=flag)

        document = {
            "cells": [
                flagged_cell("batch", "default", "q1", True),
                flagged_cell("pruning", "sorted", "q6", True),
                flagged_cell("pruning", "default", "q1", False),
                flagged_cell("rules", "default", "q3", True),
            ]
        }
        pruning = render_tables.flagged(document, "pruning")
        self.assertIn("quarry default Q6 (sorted data)", pruning)
        self.assertNotIn("Q1", pruning)
        self.assertNotIn("Q3", pruning)
        batch = render_tables.flagged(document, "batch")
        self.assertIn("quarry default Q1 (default data)", batch)
        self.assertNotIn("Q6", batch)
        self.assertIn("stayed at or below", render_tables.flagged(document, "spill"))

    def test_a_cell_with_a_null_load_flag_is_not_flagged_and_is_said_to_have_no_load_figure(self):
        timed_out = {
            "engine": "quarry", "config": "batch_size=1", "query": "q5", "dataset": "default", "group": "batch",
            "status": "timeout", "error": "killed", "load_flag": None,
        }
        text = render_tables.flagged({"cells": [timed_out, cell("quarry", "default", "q1", 5.0, group="batch")]}, "batch")
        self.assertIn("stayed at or below", text)
        self.assertNotIn("Q5", text)
        self.assertIn("no load figure", text)

    def test_a_prose_number_is_rendered_from_the_results_and_checked(self):
        self.result("main-20260101T000000Z.json", main_result(25.0))
        self.readme.write_text("The ratio is <!-- n:main.q1.ratio -->0<!-- /n --> for Q1.\n")
        self.assertEqual(self.run_tables("--check"), 1)
        self.assertEqual(self.run_tables(), 0)
        self.assertIn("<!-- n:main.q1.ratio -->2.50<!-- /n -->", self.readme.read_text())
        self.assertEqual(self.run_tables("--check"), 0)
        self.readme.write_text(self.readme.read_text().replace("2.50", "3.50"))
        self.assertEqual(self.run_tables("--check"), 1)

    def test_a_prose_number_no_result_produces_is_an_error(self):
        self.result("main-20260101T000000Z.json", main_result(25.0))
        self.readme.write_text("<!-- n:main.q9.ratio -->1<!-- /n -->\n")
        self.assertEqual(self.run_tables(), 1)

    def test_the_docs_directory_is_checked_too(self):
        (self.root / "docs" / "design").mkdir(parents=True)
        doc = self.root / "docs" / "design" / "0001-x.md"
        doc.write_text("<!-- n:main.q1.ratio -->0<!-- /n -->\n")
        self.result("main-20260101T000000Z.json", main_result(25.0))
        self.assertEqual(self.run_tables("--check"), 1)
        self.assertEqual(self.run_tables(), 0)
        self.assertIn("2.50", doc.read_text())

    def test_a_measurement_in_prose_needs_a_result_or_a_label(self):
        text = "It took 113 s to build.\n\nThe flush size is 4 KiB.\n\nAbout 68 GB <!-- obs: footprint -->\n"
        self.assertEqual([n for n, _ in render_tables.unlabelled_numbers(text)], [1, 3])
        labelled = "It took 113 s to build. <!-- obs: timed once by hand -->\nThe flush is 4 KiB.\n"
        self.assertEqual(render_tables.unlabelled_numbers(labelled), [])

    def test_numbers_in_code_fences_bound_blocks_and_markers_are_not_flagged(self):
        text = (
            "```sh\nquarry --memory-limit 64KiB 12 ms\n```\n\n"
            "<!-- bench:main -->\n| Q1 | 12 ms |\n<!-- /bench:main -->\n\n"
            "Q1 takes <!-- n:main.q1.quarry -->12<!-- /n --> ms, `64 KiB` in code.\n"
        )
        self.assertEqual(render_tables.unlabelled_numbers(text), [])

    def test_check_fails_on_an_unlabelled_measurement_in_a_doc(self):
        self.readme.write_text("It took 113 s to build.\n")
        self.assertEqual(self.run_tables("--check"), 1)
        self.readme.write_text("It took 113 s to build. <!-- obs: timed once by hand -->\n")
        self.assertEqual(self.run_tables("--check"), 0)

    def test_an_unknown_table_is_an_error(self):
        self.readme.write_text("<!-- bench:nope -->\n<!-- /bench:nope -->\n")
        self.assertEqual(self.run_tables(), 1)


if __name__ == "__main__":
    unittest.main()
