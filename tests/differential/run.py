#!/usr/bin/env python3
"""Seeded differential test of quarry against DuckDB (spec section 5.4).

  uv run tests/differential/run.py --quarry build/release/tools/quarry --seeds 0:300
  uv run tests/differential/run.py --quarry build/release/tools/quarry --seed 17 --keep

For every seed the script generates 2 to 4 tables (gen_data.py), writes them as Parquet files into a
temporary directory, loads the same files into DuckDB, generates 5 queries (gen_query.py) and runs
each on both engines with randomly chosen options (batch size, memory limit, one disabled
optimizer rule). The results are compared by compare.py. The seeds in regression_seeds.txt always
run first.

Exit status 0 when every seed matches; 1 otherwise, after printing for each mismatch the seed, the
SQL, the engine options and a row diff; 3 when the script itself failed.

How the outcomes are judged:
  * both engines return a result: compare.diff must be empty;
  * both fail with an overflow-class error: agreement (counted by the pair of messages);
  * quarry reports that the query does not fit the memory limit (a sort needs all its rows in
    memory, and a deeply nested join can run out of budget): that is the documented refusal, not a
    wrong answer. The query is then run again without the limit, and that run is compared. The
    number of refusals is reported;
  * anything else (one engine fails, a different kind of error, a crash, a timeout) is a mismatch.

A query whose FROM and WHERE alone return more than --max-rows rows is skipped, so that a skewed key
cannot turn one query into minutes of work; the number is reported.
"""

import argparse
import collections
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import duckdb

import compare
import gen_data
import gen_query

HERE = Path(__file__).resolve().parent
REGRESSION_FILE = HERE / "regression_seeds.txt"
QUERY_TIMEOUT_SECONDS = 120
# The refusals under a memory limit that are by design, and nothing else: a sort needs its whole
# input in memory, and a join partition that still does not fit after the deepest split (rows that
# share too much of their key hash) is refused. Any other "resource exhausted" under the limit, such
# as a hash aggregate that should have spilled, is a bug and a mismatch.
REFUSAL_REASONS = {
    "sort over budget": re.compile(r"resource exhausted: Sort #\d+ needs \d+ bytes, budget \d+, in use \d+$"),
    "join partition at maximum depth": re.compile(
        r"resource exhausted: HashJoin #\d+ needs \d+ bytes, budget \d+, in use \d+: "
        r"a partition does not fit although the input was split by hash down to partition depth"
    ),
}


def refusal_reason(message):
    """The documented reason for a refusal under a memory limit, or None if it is not one."""
    text = " ".join(message.split())
    for reason, pattern in REFUSAL_REASONS.items():
        if pattern.search(text):
            return reason
    return None


def read_regression_seeds(path=REGRESSION_FILE):
    seeds = []
    if path.exists():
        for line in path.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                seeds.append(int(line))
    return seeds


def parse_range(text):
    low, _, high = text.partition(":")
    return range(int(low), int(high))


class Stats:
    def __init__(self):
        self.queries = 0
        self.compared = 0
        self.nonempty = 0
        self.both_error = collections.Counter()
        self.budget_refusals = collections.Counter()  # reason -> re-runs
        self.skipped_big = 0
        self.failures = 0

    def summary(self):
        lines = [
            f"queries run: {self.queries}",
            f"compared results: {self.compared} ({self.nonempty} with at least one row)",
            f"both engines raised an overflow-class error: {sum(self.both_error.values())}",
            f"quarry refused for the memory limit, rerun without it: {sum(self.budget_refusals.values())}",
            f"skipped for size: {self.skipped_big}",
        ]
        for reason, count in self.budget_refusals.most_common():
            lines.append(f"    {count}x re-run, reason: {reason}")
        assert set(self.budget_refusals) <= set(REFUSAL_REASONS), "a re-run for an undocumented reason"
        for (quarry_message, duck_message), count in self.both_error.most_common():
            lines.append(f"    {count}x quarry: {quarry_message} | duckdb: {duck_message}")
        return "\n".join(lines)


def run_quarry(quarry, data_dir, sql, args):
    command = [str(quarry), "query", "--data", str(data_dir), "-c", sql, "--format", "json", *args]
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=QUERY_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        return "timeout", f"no answer in {QUERY_TIMEOUT_SECONDS} s"
    if done.returncode == 0:
        return "ok", compare.parse_quarry(done.stdout)
    if done.returncode < 0 or done.returncode >= 128:
        return "crash", f"exit status {done.returncode}: {done.stderr.strip()[-400:]}"
    return "error", done.stderr.strip()


def run_duckdb(con, sql):
    try:
        cursor = con.execute(sql)
        return "ok", compare.from_duckdb(cursor.description, cursor.fetchall())
    except duckdb.Error as error:
        return "error", str(error).strip()


def short(message):
    return " ".join(message.split())[:160]


def format_failure(seed, query, args, reasons, data_dir):
    lines = [
        f"MISMATCH seed {seed}, query {query.index}",
        f"  sql:     {query.sql}",
        f"  options: {' '.join(args)}",
        f"  data:    {data_dir}",
    ]
    lines += [f"  {line}" for line in reasons]
    lines.append(
        f"  repeat:  uv run tests/differential/run.py --quarry <quarry> --seed {seed} --keep"
    )
    return "\n".join(lines)


def check_query(quarry, con, data_dir, query, stats, max_rows):
    """Runs one query on both engines. Returns a list of problem lines (empty when they agree)."""
    stats.queries += 1
    kind, count = run_duckdb(con, query.count_sql)
    if kind == "ok" and count.rows[0][0] > max_rows:
        stats.skipped_big += 1
        return []

    args = query.options.cli_args()
    expected_kind, expected = run_duckdb(con, query.sql)
    got_kind, got = run_quarry(quarry, data_dir, query.sql, args)

    if got_kind in ("crash", "timeout"):
        return [f"quarry {got_kind}: {got}"]

    if got_kind == "error" and query.options.memory_limit and "resource exhausted" in got:
        reason = refusal_reason(got)
        if reason is None:
            return [f"quarry refused under the memory limit for a reason that is not by design:\n    quarry: {short(got)}"]
        stats.budget_refusals[reason] += 1
        index = args.index("--memory-limit")
        del args[index : index + 2]
        got_kind, got = run_quarry(quarry, data_dir, query.sql, args)
        if got_kind in ("crash", "timeout"):
            return [f"quarry {got_kind} (rerun without the memory limit): {got}"]

    if expected_kind == "error" and got_kind == "error":
        if compare.is_overflow_error(expected) and compare.is_overflow_error(got):
            stats.both_error[(short(got), short(expected))] += 1
            return []
        return [f"both engines failed, but not both with an overflow:\n    duckdb: {short(expected)}\n    quarry: {short(got)}"]
    if expected_kind == "error":
        return [f"duckdb failed, quarry returned {len(got.rows)} rows:\n    duckdb: {short(expected)}"]
    if got_kind == "error":
        return [f"quarry failed, duckdb returned {len(expected.rows)} rows:\n    quarry: {short(got)}"]

    stats.compared += 1
    if expected.rows:
        stats.nonempty += 1
    return compare.diff(expected, got, ordered=query.ordered, approx=query.approx)


def run_seed(quarry, seed, work_dir, stats, max_rows, keep):
    dataset = gen_data.generate(seed)
    data_dir = work_dir / f"seed-{seed}"
    dataset.write(data_dir)
    con = duckdb.connect()
    con.execute("SET threads = 1")
    for table in dataset.tables:
        path = (data_dir / f"{table.name}.parquet").as_posix()
        con.execute(f"CREATE TABLE {table.name} AS SELECT * FROM read_parquet('{path}')")
    failures = []
    for query in gen_query.generate_queries(seed, dataset):
        problems = check_query(quarry, con, data_dir, query, stats, max_rows)
        if problems:
            stats.failures += 1
            failures.append(format_failure(seed, query, query.options.cli_args(), problems, data_dir if keep else "(removed; use --keep)"))
    con.close()
    if not keep:
        shutil.rmtree(data_dir, ignore_errors=True)
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--quarry", required=True, help="path to the quarry executable")
    parser.add_argument("--seeds", default="0:300", help="half-open range LOW:HIGH (default 0:300)")
    parser.add_argument("--seed", type=int, help="run this one seed (and no regression seeds)")
    parser.add_argument("--keep", action="store_true", help="keep the generated Parquet files")
    parser.add_argument("--stop-on-failure", action="store_true", help="stop at the first mismatching seed")
    parser.add_argument("--max-rows", type=int, default=400_000, help="skip a query whose FROM/WHERE returns more rows")
    parser.add_argument("--work-dir", help="where to write the data (default: a new temporary directory)")
    args = parser.parse_args()

    quarry = Path(args.quarry)
    if not quarry.exists():
        sys.exit(f"run.py: {quarry} does not exist")
    if args.seed is not None:
        seeds = [args.seed]
    else:
        seeds = read_regression_seeds()
        regression = len(seeds)
        seeds += [s for s in parse_range(args.seeds) if s not in set(seeds)]
    work_dir = Path(args.work_dir) if args.work_dir else Path(tempfile.mkdtemp(prefix="quarry-differential-"))
    work_dir.mkdir(parents=True, exist_ok=True)

    stats = Stats()
    started = time.monotonic()
    failed_seeds = []
    try:
        for done, seed in enumerate(seeds, 1):
            failures = run_seed(quarry, seed, work_dir, stats, args.max_rows, args.keep)
            if failures:
                failed_seeds.append(seed)
                print("\n".join(failures), flush=True)
                if args.stop_on_failure:
                    break
            if done % 25 == 0:
                print(f"... {done}/{len(seeds)} seeds, {time.monotonic() - started:.0f} s", flush=True)
    finally:
        if not args.keep:
            shutil.rmtree(work_dir, ignore_errors=True)
    elapsed = time.monotonic() - started
    print(stats.summary())
    if failed_seeds:
        print(f"FAILED: {len(failed_seeds)} seeds: {failed_seeds}  ({elapsed:.0f} s)")
        return 1
    print(f"OK: {len(seeds)} seeds, {stats.queries} queries, no mismatch ({elapsed:.0f} s)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception:  # a broken run is not a mismatch: scripts/run-flaws.sh tells them apart by status
        import traceback

        traceback.print_exc()
        sys.exit(3)
