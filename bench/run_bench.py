#!/usr/bin/env python3
"""Runs a benchmark suite as docs/benchmarks.md specifies and writes bench/results/<suite>-<UTC>.json.

  uv run bench/run_bench.py --suite {main,ablation,spill,smoke} --data bench/data/sf1

The ablation suite also reads the sorted export, by default the sibling `<data>-sorted`
(bench/data/sf1-sorted). The smoke suite is the main suite on the SF0.01 test data, for CI.

Before any cell is timed, every variant of quarry the suite runs (batch sizes, rules off, memory
limits) is run once and its answer compared with DuckDB's, so a wrong answer is never published as a
time. A variant whose check hits the execution timeout is recorded as a timeout without being timed.

The script refuses (exit status 2) to start, and again to write, when `git status --porcelain`
lists anything outside bench/results/: a result must come from committed code and a committed
protocol. Every path in a result is relative to the repository root.

Exit status: 0 done, 1 a failed check or a failed run, 2 refused or a usage error.
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
SCRIPT_ROOT = SCRIPT_DIR.parent

RUNS = 5  # measured runs after one warm-up
EXECUTION_TIMEOUT_S = 600
CELL_LIMIT_S = 1200  # a cell's process is killed after this long: the 600 s cap above is cooperative
LOAD_LIMIT = 1.5
LOAD_WAIT_S = 120
RESULTS_DIR = "bench/results"
QUERIES = ["q1", "q3", "q5", "q6"]
QUERIES_DIR = "tests/tpch/queries"
AGG_QUERY = "agg"
AGG_SQL = "SELECT l_orderkey, SUM(l_extendedprice) FROM lineitem GROUP BY l_orderkey"
BATCH_SIZES = [1, 64, 1024, 2048, 8192]
BUDGETS = [("unlimited", None), ("256MB", "256MB"), ("64MB", "64MB"), ("16MB", "16MB")]
SUITES = ["main", "ablation", "spill", "smoke"]


class Refused(Exception):
    """The run must not happen or must not be written; the message says why."""


# The clean-tree rule.


def dirty_paths(root):
    """Paths with an uncommitted change (staged, unstaged or untracked) outside bench/results/."""
    done = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain=v1", "-z", "--untracked-files=all"],
        capture_output=True,
        text=True,
    )
    if done.returncode != 0:
        raise Refused(f"git status failed in the repository root: {done.stderr.strip()}")
    entries = done.stdout.split("\0")
    paths = []
    i = 0
    while i < len(entries):
        entry = entries[i]
        i += 1
        if len(entry) < 4:
            continue
        status, path = entry[:2], entry[3:]
        paths.append(path)
        if "R" in status or "C" in status:  # a rename or copy lists its source next
            if i < len(entries) and entries[i]:
                paths.append(entries[i])
            i += 1
    prefix = RESULTS_DIR + "/"
    return sorted(p for p in paths if not p.startswith(prefix))


def require_clean_tree(root, when):
    dirty = dirty_paths(root)
    if dirty:
        raise Refused(
            f"refusing to {when}: the working tree has uncommitted changes outside {RESULTS_DIR}/:\n"
            + "\n".join(f"  {p}" for p in dirty)
            + "\nCommit them (the protocol and the scripts included) and run again."
        )


# What the result says about the machine and the build.


def command_output(*command):
    try:
        done = subprocess.run(command, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def machine_info():
    if sys.platform == "darwin":
        model = command_output("sysctl", "-n", "machdep.cpu.brand_string")
        memory = int(command_output("sysctl", "-n", "hw.memsize") or 0)
        os_name = f"macOS {platform.mac_ver()[0]}"
    else:
        model = platform.processor() or platform.machine()
        for line in Path("/proc/cpuinfo").read_text().splitlines() if Path("/proc/cpuinfo").exists() else []:
            if line.startswith("model name"):
                model = line.split(":", 1)[1].strip()
                break
        memory = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
        os_name = f"{platform.system()} {platform.release()}"
    return {"model": model, "cores": os.cpu_count(), "memory_bytes": memory, "os": os_name}


def read_cmake_cache(build_dir):
    cache = {}
    path = Path(build_dir) / "CMakeCache.txt"
    if not path.exists():
        raise Refused(f"{path.name} not found in the build directory: build quarry first")
    for line in path.read_text().splitlines():
        match = re.match(r"^([A-Za-z0-9_]+):[A-Z]+=(.*)$", line)
        if match:
            cache[match.group(1)] = match.group(2)
    return cache


def compiler_info(cache):
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    flags = " ".join(
        f for f in (cache.get("CMAKE_CXX_FLAGS", ""), cache.get(f"CMAKE_CXX_FLAGS_{build_type.upper()}", "")) if f
    )
    version = command_output(cache.get("CMAKE_CXX_COMPILER", "c++"), "--version").splitlines()
    return {
        "version": version[0] if version else "unknown",
        "build_type": build_type,
        "flags": flags,
        "march_native": "-march=native" in flags,
    }


def arrow_info(cache):
    """The contents of quarry-arrow-build.txt in the Arrow install the build used."""
    prefix = cache.get("QUARRY_ARROW_PREFIX", "")
    path = Path(prefix) / "quarry-arrow-build.txt"
    if not prefix or not path.exists():
        raise Refused("quarry-arrow-build.txt not found in the Arrow install the build used")
    info = {"flags": [], "libraries": []}
    section = None
    for line in path.read_text().splitlines():
        if line in ("flags:", "libraries:"):
            section = line[:-1]
        elif section and line.startswith("  "):
            info[section].append(line.strip())
        elif ":" in line and not line.startswith(" "):
            section = None
            key, value = line.split(":", 1)
            info[key.strip()] = value.strip()
    return info


def data_info(directory):
    """Scale factor, row-group size and sortedness of a TPC-H export, read from its Parquet files."""
    import pyarrow.parquet as pq

    directory = Path(directory)
    orders = pq.ParquetFile(directory / "orders.parquet").metadata.num_rows
    lineitem = pq.ParquetFile(directory / "lineitem.parquet")
    meta = lineitem.metadata
    shipdate = lineitem.schema_arrow.get_field_index("l_shipdate")
    bounds = []
    for group in range(meta.num_row_groups):
        stats = meta.row_group(group).column(shipdate).statistics
        bounds.append((stats.min, stats.max))
    sorted_lineitem = all(bounds[i][1] <= bounds[i + 1][0] for i in range(len(bounds) - 1))
    return {
        "scale_factor": round(orders / 1_500_000, 4),
        "lineitem_rows": meta.num_rows,
        "row_groups": meta.num_row_groups,
        "row_group_rows": meta.row_group(0).num_rows,
        "compression": meta.row_group(0).column(0).compression.lower(),
        "sorted_lineitem": sorted_lineitem,
        "parquet_bytes": sum(p.stat().st_size for p in directory.glob("*.parquet")),
    }


def relative(path, root):
    """`path` as the repository-relative string a result may hold; outside the repository: its name."""
    path = Path(path).resolve()
    try:
        return path.relative_to(Path(root).resolve()).as_posix()
    except ValueError:
        return path.name


# The cells of each suite.


def cell(engine, config, query, dataset, group, args=(), threads=None):
    return {
        "engine": engine,
        "config": config,
        "query": query,
        "dataset": dataset,
        "group": group,
        "args": list(args),
        "threads": threads,
    }


def duckdb_cells(queries, dataset, group="engines"):
    return [
        cell("duckdb", f"threads={t}", q, dataset, group, threads=t) for t in ("1", "default") for q in queries
    ]


def suite_cells(suite):
    if suite in ("main", "smoke"):
        cells = []
        for q in QUERIES:
            cells.append(cell("quarry", "default", q, "default", "engines"))
            cells.extend(duckdb_cells([q], "default"))
        return cells
    if suite == "ablation":
        cells = []
        for size in BATCH_SIZES:
            for q in QUERIES:
                cells.append(cell("quarry", f"batch_size={size}", q, "default", "batch", ["--batch-size", str(size)]))
        for dataset in ("sorted", "default"):
            for q in ("q1", "q6"):
                cells.append(cell("quarry", "default", q, dataset, "pruning"))
                cells.append(
                    cell("quarry", "without prune_row_groups", q, dataset, "pruning", ["--disable-rule", "prune_row_groups"])
                )
        for q in ("q1", "q6"):
            cells.append(cell("duckdb", "threads=1", q, "sorted", "pruning", threads="1"))
        for q in QUERIES:
            cells.append(cell("quarry", "without prune_columns", q, "default", "rules", ["--disable-rule", "prune_columns"]))
        for q in ("q3", "q5"):
            cells.append(cell("quarry", "default", q, "default", "rules"))
            cells.append(cell("quarry", "without order_joins", q, "default", "rules", ["--disable-rule", "order_joins"]))
        for q in ("q1", "q6"):
            cells.append(cell("quarry", "default", q, "default", "rules"))
        return cells
    if suite == "spill":
        cells = []
        for q in ("q3", "q5", AGG_QUERY):
            for name, limit in BUDGETS:
                cells.append(
                    cell("quarry", name, q, "default", "spill", ["--memory-limit", limit] if limit else [])
                )
        return cells
    raise Refused(f"unknown suite {suite}")


# Running one cell, in a fresh process.


def summarize(raw):
    """A cell's statistics from the timing JSON a worker wrote."""
    result = {}
    if "error" in raw:
        result["status"] = "timeout" if raw.get("timed_out") else "error"
        result["error"] = raw["error"]
    else:
        result["status"] = "ok"
    runs = [r["ns"] / 1e6 for r in raw["runs"]]
    loads = [r["load_1m"] for r in raw["runs"]]
    warmup = raw.get("warmup")  # null when the warm-up itself failed
    entries = ([warmup] if warmup else []) + raw["runs"]
    if result["status"] == "ok":
        result.update(
            {
                "runs_ms": [round(r, 3) for r in runs],
                "median_ms": round(statistics.median(runs), 3),
                "min_ms": round(min(runs), 3),
                "max_ms": round(max(runs), 3),
            }
        )
    # A figure that was never measured is null, never a zero that reads like a measurement.
    result.update(
        {
            "warmup_ms": round(warmup["ns"] / 1e6, 3) if warmup else None,
            "warmup_load_1m": warmup["load_1m"] if warmup else None,
            "load_1m": loads,
            "waited_s": [r["waited_s"] for r in raw["runs"]],
            "load_flag": any(e["load_1m"] > LOAD_LIMIT for e in entries) if entries else None,
            "peak_rss_bytes": raw["peak_rss_bytes"],
            "idle_rss_bytes": raw["idle_rss_bytes"],
            "rows": raw["rows"],
        }
    )
    for key in ("pool_peak_bytes", "scan_pool_peak_bytes", "spilled_bytes", "threads"):
        if key in raw:
            result[key] = raw[key]
    return result


def failed_cell(spec, status, error):
    """The record of a cell that has no timing. Nothing was measured, so the load flag is null."""
    record = {k: spec[k] for k in ("engine", "config", "query", "dataset", "group")}
    record.update({"status": status, "error": error, "load_flag": None})
    return record


def run_cell(spec, ctx):
    data = ctx["data"][spec["dataset"]]
    sql = ctx["sql"][spec["query"]]
    with tempfile.TemporaryDirectory() as scratch:
        out = Path(scratch) / "timing.json"
        wait = ["--load-limit", str(LOAD_LIMIT), "--load-wait", str(LOAD_WAIT_S)] if ctx["wait_for_load"] else []
        if spec["engine"] == "quarry":
            command = [
                str(ctx["quarry"]), "query", "--data", str(data), "-c", sql, "--repeat", str(RUNS),
                "--timing-json", str(out), "--timeout", f"{EXECUTION_TIMEOUT_S}s", *spec["args"], *wait,
            ]
        else:
            command = [
                sys.executable, str(SCRIPT_DIR / "duckdb_worker.py"), "--data", str(data), "--sql", sql,
                "--threads", spec["threads"], "--repeat", str(RUNS), "--timeout", str(EXECUTION_TIMEOUT_S),
                "--out", str(out), *wait,
            ]
        proc = subprocess.Popen(command, cwd=ctx["root"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            _, stderr = proc.communicate(timeout=ctx["cell_limit_s"])
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.communicate()
            return failed_cell(
                spec, "timeout", f"the process was killed after the cell limit of {ctx['cell_limit_s']} s"
            )
        done = subprocess.CompletedProcess(command, proc.returncode, "", stderr)
        if not out.exists():
            # The process died without writing its timing: a crash is the result of its cell.
            code = done.returncode
            how = f"killed by signal {-code}" if code < 0 else f"exit status {code}"
            return failed_cell(
                spec, "crashed",
                f"the process died without writing a timing ({how}): {done.stderr.strip()[-400:]}",
            )
        raw = json.loads(out.read_text())
    cell_result = {k: spec[k] for k in ("engine", "config", "query", "dataset", "group")}
    cell_result.update(summarize(raw))
    return cell_result


# The check before the numbers: every variant of quarry answers as DuckDB does.


def variants(specs):
    """The distinct (dataset, query, quarry arguments) of the quarry cells, in cell order."""
    seen = {}
    for spec in specs:
        if spec["engine"] == "quarry":
            seen.setdefault(variant_key(spec), spec)
    return list(seen.values())


def variant_key(spec):
    return (spec["dataset"], spec["query"], tuple(spec["args"]))


def check_answers(ctx, specs):
    """Runs each variant once, with the arguments its cells use, and compares the answer with
    DuckDB's on the same data. A wrong answer raises AnswerMismatch before anything is timed.

    Returns {variant key: reason} for the variants whose check did not finish within the execution
    timeout (or the cell limit): the caller records those cells as timeouts without timing them, as
    the timed run would end the same way. A variant that fails otherwise (a memory limit that
    refuses the query, say) is not in the result: its timed run records the failure as its cell."""
    sys.path.insert(0, str(SCRIPT_ROOT / "tests" / "differential"))
    import compare
    import duckdb

    connections = {}
    expected_answers = {}

    def expected(dataset, query):
        if dataset not in connections:
            con = duckdb.connect()
            con.execute("SET threads = 1")
            for path in sorted(Path(ctx["data"][dataset]).glob("*.parquet")):
                literal = path.as_posix().replace("'", "''")
                con.execute(f"CREATE VIEW {path.stem} AS SELECT * FROM read_parquet('{literal}')")
            connections[dataset] = con
        if (dataset, query) not in expected_answers:
            cursor = connections[dataset].execute(ctx["sql"][query])
            expected_answers[(dataset, query)] = compare.from_duckdb(cursor.description, cursor.fetchall())
        return expected_answers[(dataset, query)]

    unfinished = {}
    todo = variants(specs)
    for n, spec in enumerate(todo, 1):
        label = f"{spec['query']} {spec['config']} ({spec['dataset']})"
        print(f"[check {n}/{len(todo)}] {label}", file=sys.stderr)
        command = [
            str(ctx["quarry"]), "query", "--data", str(ctx["data"][spec["dataset"]]), "-c", ctx["sql"][spec["query"]],
            "--format", "json", "--timeout", f"{EXECUTION_TIMEOUT_S}s", *spec["args"],
        ]
        try:
            done = subprocess.run(
                command, cwd=ctx["root"], capture_output=True, text=True, timeout=ctx["cell_limit_s"]
            )
        except subprocess.TimeoutExpired:
            unfinished[variant_key(spec)] = f"the answer check was killed after the cell limit of {ctx['cell_limit_s']} s"
            continue
        if done.returncode != 0:
            if "timeout of" in done.stderr:
                unfinished[variant_key(spec)] = (
                    f"the answer check ended at the execution timeout of {EXECUTION_TIMEOUT_S} s"
                )
            continue
        want = expected(spec["dataset"], spec["query"])
        actual = compare.parse_quarry(done.stdout)
        approx = frozenset(i for i, (_, t) in enumerate(want.columns) if t == "DOUBLE")
        ordered = spec["query"] != AGG_QUERY  # the aggregate has no ORDER BY
        problems = compare.diff(want, actual, ordered=ordered, approx=approx)
        if problems:
            raise AnswerMismatch(
                f"{label}: the answer of quarry differs from DuckDB's:\n  " + "\n  ".join(problems)
            )
    for con in connections.values():
        con.close()
    return unfinished


class AnswerMismatch(Exception):
    pass


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--suite", required=True, choices=SUITES)
    parser.add_argument("--data", required=True, type=Path, help="the default export, e.g. bench/data/sf1")
    parser.add_argument("--sorted-data", type=Path, help="the lineitem-sorted export (default: <data>-sorted)")
    parser.add_argument("--repo-root", type=Path, default=SCRIPT_ROOT, help="the repository (default: this one)")
    parser.add_argument("--build-dir", type=Path, default=Path("build/release"))
    parser.add_argument("--quarry", type=Path, help="the quarry binary (default: <build-dir>/tools/quarry)")
    parser.add_argument("--no-build", action="store_true", help="do not run cmake --build first")
    parser.add_argument("--cell-limit", type=int, default=CELL_LIMIT_S, help="seconds before a cell's process is killed")
    parser.add_argument(
        "--no-load-wait",
        action="store_true",
        help="record the load average but never wait for it (for tests; the smoke suite always does this)",
    )
    args = parser.parse_args(argv)

    root = args.repo_root.resolve()
    build_dir = args.build_dir if args.build_dir.is_absolute() else root / args.build_dir
    quarry = args.quarry or build_dir / "tools" / "quarry"
    data = args.data if args.data.is_absolute() else root / args.data
    sorted_data = args.sorted_data or data.parent / f"{data.name}-sorted"
    if args.sorted_data and not args.sorted_data.is_absolute():
        sorted_data = root / args.sorted_data

    try:
        require_clean_tree(root, "start")
        if not args.no_build:
            subprocess.run(["cmake", "--build", str(build_dir), "--target", "quarry"], check=True, capture_output=True)
        if not Path(quarry).exists():
            raise Refused("the quarry binary does not exist: build it first")
        datasets = {"default": data}
        if args.suite == "ablation":
            datasets["sorted"] = sorted_data
        for name, path in datasets.items():
            if not (path / "lineitem.parquet").exists():
                raise Refused(f"no TPC-H export at {relative(path, root)} ({name} data)")

        sql = {q: (root / QUERIES_DIR / f"{q}.sql").read_text(encoding="utf-8") for q in QUERIES}
        sql[AGG_QUERY] = AGG_SQL
        ctx = {
            "root": root,
            "quarry": quarry,
            "data": datasets,
            "sql": sql,
            "wait_for_load": args.suite != "smoke" and not args.no_load_wait,
            "cell_limit_s": args.cell_limit,
        }

        import duckdb

        cache = read_cmake_cache(build_dir)
        document = {
            "schema_version": 1,
            "suite": args.suite,
            "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "repo_commit": subprocess.run(
                ["git", "-C", str(root), "rev-parse", "HEAD"], capture_output=True, text=True, check=True
            ).stdout.strip(),
            "tree_dirty": False,
            "machine": machine_info(),
            "compiler": compiler_info(cache),
            "arrow": arrow_info(cache),
            "duckdb_version": duckdb.__version__,
            "data": {name: dict(data_info(p), path=relative(p, root)) for name, p in datasets.items()},
            "protocol": {
                "runs": RUNS,
                "warmups": 1,
                "load_limit": LOAD_LIMIT,
                "load_wait_s": LOAD_WAIT_S if ctx["wait_for_load"] else 0,
                "execution_timeout_s": EXECUTION_TIMEOUT_S,
                "cell_limit_s": args.cell_limit,
            },
            "queries": {q: {"sha256": hashlib.sha256(s.encode()).hexdigest()} for q, s in sql.items()},
            "cells": [],
        }
        document["queries"][AGG_QUERY]["sql"] = AGG_SQL

        specs = suite_cells(args.suite)
        print("checking quarry's answers against DuckDB's ...", file=sys.stderr)
        unfinished = check_answers(ctx, specs)

        for n, spec in enumerate(specs, 1):
            if variant_key(spec) in unfinished:
                result = failed_cell(
                    spec, "timeout", unfinished[variant_key(spec)] + "; the cell was not timed"
                )
            else:
                result = run_cell(spec, ctx)
            document["cells"].append(result)
            shown = f"{result['median_ms']:.1f} ms" if result["status"] == "ok" else result["status"]
            loads = [result.get("warmup_load_1m"), *result.get("load_1m", [])]
            print(
                f"[{n}/{len(specs)}] {spec['engine']} {spec['config']} {spec['query']} ({spec['dataset']}): "
                f"{shown}, load {max([x for x in loads if x is not None], default='n/a')}",
                file=sys.stderr,
            )

        # The tree may have changed while the suite ran.
        require_clean_tree(root, "write the result")
        stamp = document["timestamp_utc"].replace("-", "").replace(":", "")
        target = root / RESULTS_DIR / f"{args.suite}-{stamp}.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        text = json.dumps(document, indent=1) + "\n"
        target.write_text(text, encoding="utf-8")
        print(relative(target, root))
        return 0
    except Refused as refusal:
        print(f"run_bench: {refusal}", file=sys.stderr)
        return 2
    except AnswerMismatch as mismatch:
        print(f"run_bench: {mismatch}\nnothing was timed and nothing was written", file=sys.stderr)
        return 1
    except (RuntimeError, subprocess.CalledProcessError) as failure:
        print(f"run_bench: {failure}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
