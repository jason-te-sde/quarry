"""Tests of bench/run_bench.py: when it refuses, and what the result file holds.

Run from the repository root:  uv run python -m unittest discover -s bench -v

Each test makes a scratch git repository holding only the committed TPC-H query files and an empty
bench/results/, and points the script at it with --repo-root. The data (tests/data/tpch-sf0.01) and
the quarry binary (tools/quarry in build/release, or in $QUARRY_BUILD_DIR) are the real ones, outside the
scratch repository. A missing binary fails the tests; it does not skip them.
"""

import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SCRIPT = REPO / "bench" / "run_bench.py"
DATA = REPO / "tests" / "data" / "tpch-sf0.01"
BUILD_DIR = (REPO / os.environ.get("QUARRY_BUILD_DIR", "build/release")).resolve()
QUARRY = BUILD_DIR / "tools" / "quarry"


# The start of an absolute path on macOS, Linux or Windows, built in pieces so that this file does not
# itself look like one to scripts/check-hygiene.sh.
HOME_LIKE = r"(^|\s)(/" + "Users/|/" + "home/|/private/|/var/|[A-Za-z]:" + r"\\)"


def git(root, *args):
    return subprocess.run(
        ["git", "-C", str(root), *args], check=True, capture_output=True, text=True
    ).stdout.strip()


def make_repo(root):
    """A committed scratch repository: the four query files and an empty bench/results/."""
    shutil.copytree(REPO / "tests" / "tpch" / "queries", root / "tests" / "tpch" / "queries")
    (root / "bench" / "results").mkdir(parents=True)
    (root / "bench" / "results" / ".gitkeep").write_text("")
    git(root, "init", "-q", "-b", "main")
    git(root, "config", "user.name", "Bench Test")
    git(root, "config", "user.email", "bench-test@example.invalid")
    git(root, "add", "-A")
    git(root, "commit", "-q", "-m", "scratch")


def write_script(path, source):
    """An executable `path` that runs `source` with this Python. A shell launcher, because the
    interpreter's own path may hold spaces, which a shebang line cannot."""
    body = path.with_name(path.name + ".py")
    body.write_text(source)
    path.write_text(f"#!/bin/sh\nexec '{sys.executable}' '{body}' \"$@\"\n")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)


class RunBenchTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(QUARRY.exists(), f"build quarry first: cmake --preset release && cmake --build --preset release ({QUARRY})")
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "repo"
        self.root.mkdir()
        make_repo(self.root)
        self.tools = Path(self.temp.name) / "tools"
        self.tools.mkdir()

    def run_bench(self, suite="smoke", quarry=None, extra=()):
        command = [
            sys.executable, str(SCRIPT), "--suite", suite, "--data", str(DATA),
            "--repo-root", str(self.root), "--build-dir", str(BUILD_DIR),
            "--quarry", str(quarry or QUARRY), "--no-build", "--no-load-wait", *extra,
        ]
        return subprocess.run(command, capture_output=True, text=True, timeout=600)

    def results(self):
        return sorted((self.root / "bench" / "results").glob("*.json"))

    # The dirty-tree refusal.

    def test_refuses_a_modified_tracked_file(self):
        path = self.root / "tests" / "tpch" / "queries" / "q1.sql"
        path.write_text(path.read_text() + "\n-- edited\n")
        done = self.run_bench()
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("tests/tpch/queries/q1.sql", done.stderr)
        self.assertEqual(self.results(), [])

    def test_refuses_an_untracked_file(self):
        (self.root / "notes.txt").write_text("not committed")
        done = self.run_bench()
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("notes.txt", done.stderr)
        self.assertEqual(self.results(), [])

    def test_refuses_a_staged_file(self):
        (self.root / "staged.txt").write_text("staged")
        git(self.root, "add", "staged.txt")
        done = self.run_bench()
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("staged.txt", done.stderr)

    def test_names_every_dirty_path(self):
        (self.root / "a.txt").write_text("a")
        (self.root / "b.txt").write_text("b")
        done = self.run_bench()
        self.assertEqual(done.returncode, 2)
        self.assertIn("a.txt", done.stderr)
        self.assertIn("b.txt", done.stderr)

    def test_refuses_when_the_tree_turns_dirty_during_the_run(self):
        wrapper = self.tools / "quarry-that-dirties"
        write_script(
            wrapper,
            "import os, sys\n"
            f"open({str(self.root / 'stray.txt')!r}, 'w').write('x')\n"
            f"os.execv({str(QUARRY)!r}, [{str(QUARRY)!r}] + sys.argv[1:])\n",
        )
        done = self.run_bench(quarry=wrapper)
        self.assertEqual(done.returncode, 2, done.stderr)
        self.assertIn("stray.txt", done.stderr)
        self.assertEqual(self.results(), [])

    def test_results_directory_may_be_dirty(self):
        (self.root / "bench" / "results" / "older-run.json").write_text("{}")
        done = self.run_bench()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(len(self.results()), 2)

    # What the result file holds.

    def test_result_file_content(self):
        done = self.run_bench()
        self.assertEqual(done.returncode, 0, done.stderr)
        files = self.results()
        self.assertEqual(len(files), 1)
        self.assertRegex(files[0].name, r"^smoke-\d{8}T\d{6}Z\.json$")
        text = files[0].read_text()
        result = json.loads(text)

        self.assertIs(result["tree_dirty"], False)
        self.assertEqual(result["repo_commit"], git(self.root, "rev-parse", "HEAD"))
        self.assertEqual(result["suite"], "smoke")
        for key in ("model", "cores", "memory_bytes", "os"):
            self.assertIn(key, result["machine"])
        self.assertIn("O3", result["compiler"]["flags"])
        self.assertIs(result["compiler"]["march_native"], False)
        self.assertEqual(result["arrow"]["arrow_version"], "26.0.1")
        self.assertIn("-DARROW_COMPUTE=OFF", result["arrow"]["flags"])
        self.assertRegex(result["duckdb_version"], r"^\d+\.\d+")
        self.assertEqual(result["data"]["default"]["scale_factor"], 0.01)
        self.assertIs(result["data"]["default"]["sorted_lineitem"], False)
        self.assertGreater(result["data"]["default"]["row_group_rows"], 0)

        cells = result["cells"]
        self.assertEqual(
            {(c["engine"], c["config"], c["query"]) for c in cells},
            {(e, c, q) for e, c in (("quarry", "default"), ("duckdb", "threads=1"), ("duckdb", "threads=default")) for q in ("q1", "q3", "q5", "q6")},
        )
        for cell in cells:
            self.assertEqual(cell["status"], "ok", cell)
            self.assertEqual(len(cell["runs_ms"]), 5)
            self.assertEqual(len(cell["load_1m"]), 5)
            self.assertLessEqual(cell["min_ms"], cell["median_ms"])
            self.assertLessEqual(cell["median_ms"], cell["max_ms"])
            self.assertGreater(cell["peak_rss_bytes"], 0)
            self.assertIn("warmup_ms", cell)
            self.assertIn("load_flag", cell)
            if cell["engine"] == "quarry":
                for key in ("pool_peak_bytes", "scan_pool_peak_bytes", "spilled_bytes"):
                    self.assertIn(key, cell)
            else:
                self.assertGreater(cell["idle_rss_bytes"], 0)

    def test_result_file_has_no_absolute_path(self):
        done = self.run_bench()
        self.assertEqual(done.returncode, 0, done.stderr)
        text = self.results()[0].read_text()

        def strings(value):
            if isinstance(value, str):
                yield value
            elif isinstance(value, dict):
                for key, item in value.items():
                    yield key
                    yield from strings(item)
            elif isinstance(value, list):
                for item in value:
                    yield from strings(item)

        for string in strings(json.loads(text)):
            self.assertFalse(string.startswith("/"), string)
            self.assertNotRegex(string, HOME_LIKE, string)
        for forbidden in (str(REPO), str(Path.home()), self.temp.name, str(DATA), str(QUARRY)):
            self.assertNotIn(forbidden, text)

    # Failures are results, not omissions.

    def test_a_wrong_answer_stops_the_run_and_writes_nothing(self):
        wrapper = self.tools / "quarry-with-a-wrong-answer"
        write_script(
            wrapper,
            "import os, sys\n"
            "if '--format' in sys.argv:\n"
            "    print('{\"columns\":[{\"name\":\"x\",\"type\":\"INTEGER\"}],\"rows\":[[1]]}')\n"
            "    sys.exit(0)\n"
            f"os.execv({str(QUARRY)!r}, [{str(QUARRY)!r}] + sys.argv[1:])\n",
        )
        done = self.run_bench(quarry=wrapper)
        self.assertEqual(done.returncode, 1, done.stderr)
        self.assertIn("differs from DuckDB", done.stderr)
        self.assertEqual(self.results(), [])

    def test_a_refused_cell_is_recorded_as_the_cell_result(self):
        wrapper = self.tools / "quarry-that-refuses"
        write_script(
            wrapper,
            "import json, os, sys\n"
            "args = sys.argv[1:]\n"
            "if '--timing-json' in args and '--memory-limit' in args:\n"
            "    path = args[args.index('--timing-json') + 1]\n"
            "    json.dump({'error': 'resource exhausted: Sort #3 needs 1 bytes', 'timed_out': False,\n"
            "               'warmup': {'ns': 5, 'load_1m': 0.1, 'waited_s': 0.0}, 'runs': [],\n"
            "               'idle_rss_bytes': 1, 'peak_rss_bytes': 2, 'pool_peak_bytes': 0,\n"
            "               'scan_pool_peak_bytes': 0, 'spilled_bytes': 0, 'rows': 0}, open(path, 'w'))\n"
            "    sys.exit(1)\n"
            f"os.execv({str(QUARRY)!r}, [{str(QUARRY)!r}] + args)\n",
        )
        done = self.run_bench(suite="spill", quarry=wrapper)
        self.assertEqual(done.returncode, 0, done.stderr)
        cells = json.loads(self.results()[0].read_text())["cells"]
        by_budget = {}
        for cell in cells:
            by_budget.setdefault(cell["config"], []).append(cell)
        self.assertEqual(set(by_budget), {"unlimited", "256MB", "64MB", "16MB"})
        for cell in by_budget["unlimited"]:
            self.assertEqual(cell["status"], "ok")
        self.assertEqual(len(cells), 12)
        for budget in ("256MB", "64MB", "16MB"):
            for cell in by_budget[budget]:
                self.assertEqual(cell["status"], "error")
                self.assertIn("resource exhausted", cell["error"])
                self.assertNotIn("median_ms", cell)


if __name__ == "__main__":
    unittest.main()
