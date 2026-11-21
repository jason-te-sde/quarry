#!/usr/bin/env python3
"""Times one query on DuckDB in this process: one warm-up and N measured executions.

Started by bench/run_bench.py, one fresh process per (configuration, query) cell, so that nothing
one cell did (a warm buffer cache of DuckDB's, a grown heap) reaches the next. It reads the Parquet
files of a directory through views over read_parquet, as docs/benchmarks.md says, and writes the
same JSON shape as `quarry query --repeat --timing-json`: the time of each run from the SQL text to
the result materialised as an Arrow table, the 1-minute load average before it, and the peak RSS.
`idle_rss_bytes` is the process's peak RSS just before the first query: the Python interpreter, the
duckdb and pyarrow modules and the connection.
"""

import argparse
import json
import resource
import sys
import threading
import time
from pathlib import Path


def peak_rss_bytes():
    peak = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    return peak if sys.platform == "darwin" else peak * 1024  # bytes on macOS, KiB elsewhere


def wait_for_load(limit, wait_s):
    """Reads the 1-minute load average; with a limit, waits while it is above it, for at most wait_s."""
    import os

    start = time.monotonic()
    while True:
        load = os.getloadavg()[0]
        waited = time.monotonic() - start
        if limit <= 0 or load <= limit or waited >= wait_s:
            return round(load, 2), round(waited, 1)
        time.sleep(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--data", required=True, help="directory of x.parquet files")
    parser.add_argument("--sql", required=True)
    parser.add_argument("--threads", default="default", help="a number, or 'default'")
    parser.add_argument("--repeat", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=600.0, help="seconds per execution")
    parser.add_argument("--load-limit", type=float, default=0.0)
    parser.add_argument("--load-wait", type=float, default=120.0)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    import duckdb
    import pyarrow  # noqa: F401  (imported now, so that the idle RSS includes it)

    con = duckdb.connect()
    if args.threads != "default":
        con.execute(f"SET threads = {int(args.threads)}")
    threads = int(con.execute("SELECT current_setting('threads')").fetchone()[0])
    for path in sorted(Path(args.data).glob("*.parquet")):
        literal = path.as_posix().replace("'", "''")
        con.execute(f"CREATE VIEW {path.stem} AS SELECT * FROM read_parquet('{literal}')")
    idle_rss = peak_rss_bytes()

    warmup = None
    runs = []
    rows = 0
    error = None
    timed_out = False
    for i in range(args.repeat + 1):
        load, waited = wait_for_load(args.load_limit, args.load_wait)
        interrupted = threading.Timer(args.timeout, con.interrupt)
        interrupted.start()
        start = time.perf_counter_ns()
        try:
            table = con.execute(args.sql).to_arrow_table()
            elapsed = time.perf_counter_ns() - start
        except duckdb.Error as exc:
            error = " ".join(str(exc).split())
            timed_out = isinstance(exc, duckdb.InterruptException)
            break
        finally:
            interrupted.cancel()
        rows = table.num_rows
        del table
        entry = {"ns": elapsed, "load_1m": load, "waited_s": waited}
        if i == 0:
            warmup = entry
        else:
            runs.append(entry)

    result = {}
    if error is not None:
        result["error"] = error
        result["timed_out"] = timed_out
    result.update(
        {
            "warmup": warmup or {"ns": 0, "load_1m": 0.0, "waited_s": 0.0},
            "runs": runs,
            "idle_rss_bytes": idle_rss,
            "peak_rss_bytes": peak_rss_bytes(),
            "rows": rows,
            "threads": threads,
        }
    )
    Path(args.out).write_text(json.dumps(result) + "\n", encoding="utf-8")
    return 1 if error is not None else 0


if __name__ == "__main__":
    sys.exit(main())
