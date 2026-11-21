#!/usr/bin/env python3
"""Renders the benchmark tables of the README and the docs from the committed results.

  uv run bench/render_tables.py            rewrite the tables in place
  uv run bench/render_tables.py --check    change nothing; exit 1 and show a diff when a table differs

A table lives between `<!-- bench:NAME -->` and `<!-- /bench:NAME -->`; everything between the two
is replaced. Each table is rendered from the newest result file of one suite in bench/results/
(the file name carries the UTC time), and names that file under the table. The smoke suite is never
rendered, and neither is a result recorded without waiting for the load average (--no-load-wait).
A table whose suite has no result says so.

Exit status: 0 done or in sync, 1 --check found a difference or a marker is unknown, 2 usage.
"""

import argparse
import difflib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS = Path("bench/results")
DOCS = ["README.md", "docs/benchmarks.md", "docs/where-time-goes.md"]
QUERY_LABEL = {"q1": "Q1", "q3": "Q3", "q5": "Q5", "q6": "Q6", "agg": "High-cardinality aggregate"}
MAIN_QUERIES = ["q1", "q3", "q5", "q6"]
NO_RESULT = "_No result is committed for this table yet._"


def newest(root, suite):
    """The newest usable result of a suite: (relative path, document), or None."""
    found = []
    for path in sorted((root / RESULTS).glob(f"{suite}-*.json")):
        document = json.loads(path.read_text(encoding="utf-8"))
        if document.get("protocol", {}).get("load_wait_s", 0) > 0:
            found.append((path, document))
    if not found:
        return None
    path, document = found[-1]
    return path.relative_to(root).as_posix(), document


def ms(value):
    return f"{value:,.0f}" if value >= 1000 else f"{value:.1f}"


def mib(value):
    return f"{value / 2**20:,.0f}"


def pick(cells, **want):
    for c in cells:
        if all(c.get(k) == v for k, v in want.items()):
            return c
    return None


def stat(c):
    """`median (min-max)` of a cell, or what happened to it."""
    if c is None:
        return "not run"
    if c["status"] != "ok":
        return f"error: {c['error']}" if c["status"] == "error" else c["status"]
    return f"{ms(c['median_ms'])} ({ms(c['min_ms'])}-{ms(c['max_ms'])})"


def table(header, rows):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return "\n".join(lines)


def source_line(path, document):
    return (
        f"Source: `{path}` (commit `{document['repo_commit'][:12]}`, {document['timestamp_utc']}); "
        "times are milliseconds, median (min-max) of 5 runs after 1 warm-up."
    )


def flagged(document):
    cells = [
        f"{c['engine']} {c['config']} {QUERY_LABEL[c['query']]}"
        for c in document["cells"]
        if c.get("load_flag")
    ]
    if not cells:
        return "The 1-minute load average stayed at or below 1.5 before every run of every cell."
    return "Cells that ran with a 1-minute load average above 1.5 after the wait: " + ", ".join(cells) + "."


def render_environment(root):
    found = newest(root, "main")
    if not found:
        return NO_RESULT
    path, d = found
    default = d["data"]["default"]
    return (
        f"Measured on {d['machine']['model']} ({d['machine']['cores']} cores, "
        f"{d['machine']['memory_bytes'] / 2**30:.0f} GB), {d['machine']['os']}. "
        f"quarry: {d['compiler']['version']}, {d['compiler']['build_type']} build, flags `{d['compiler']['flags']}`, "
        f"Arrow {d['arrow']['arrow_version']} (static). DuckDB {d['duckdb_version']}. "
        f"Data: TPC-H-derived, scale factor {default['scale_factor']:g}, "
        f"Parquet row groups of {default['row_group_rows']:,} rows ({default['compression']}), "
        f"{default['parquet_bytes'] / 2**20:,.0f} MiB."
    )


def render_main(root):
    found = newest(root, "main")
    if not found:
        return NO_RESULT
    path, d = found
    rows = []
    for q in MAIN_QUERIES:
        quarry = pick(d["cells"], engine="quarry", query=q)
        one = pick(d["cells"], engine="duckdb", config="threads=1", query=q)
        many = pick(d["cells"], engine="duckdb", config="threads=default", query=q)
        ratio = "n/a"
        if quarry["status"] == "ok" and one["status"] == "ok":
            ratio = f"{quarry['median_ms'] / one['median_ms']:.2f}"
        rows.append([QUERY_LABEL[q], stat(quarry), stat(one), stat(many), ratio])
    threads = pick(d["cells"], engine="duckdb", config="threads=default")
    header = [
        "Query", "quarry, 1 thread", "DuckDB, threads=1", f"DuckDB, default ({threads['threads']} threads)",
        "quarry / DuckDB (threads=1)",
    ]
    return "\n".join(
        [
            table(header, rows),
            "",
            "A ratio above 1.00 means quarry is slower than single-threaded DuckDB.",
            "",
            source_line(path, d),
            flagged(d),
        ]
    )


def render_memory(root):
    found = newest(root, "main")
    if not found:
        return NO_RESULT
    path, d = found
    rows = []
    for q in MAIN_QUERIES:
        quarry = pick(d["cells"], engine="quarry", query=q)
        one = pick(d["cells"], engine="duckdb", config="threads=1", query=q)
        many = pick(d["cells"], engine="duckdb", config="threads=default", query=q)
        if any(c["status"] != "ok" for c in (quarry, one, many)):
            rows.append([QUERY_LABEL[q]] + ["-"] * 6)
            continue
        rows.append(
            [
                QUERY_LABEL[q], mib(quarry["peak_rss_bytes"]), mib(quarry["pool_peak_bytes"]),
                mib(quarry["scan_pool_peak_bytes"]), mib(one["peak_rss_bytes"]), mib(many["peak_rss_bytes"]),
                mib(one["idle_rss_bytes"]),
            ]
        )
    header = [
        "Query", "quarry peak RSS", "quarry operator state", "quarry scan buffers",
        "DuckDB threads=1 peak RSS", "DuckDB default peak RSS", "Python + DuckDB idle RSS",
    ]
    return "\n".join(
        [
            table(header, rows),
            "",
            "MiB. The DuckDB peaks include the Python interpreter, `duckdb` and `pyarrow`; the idle column is "
            "that process's RSS just before its first query and is not subtracted. quarry's operator state and "
            "scan buffers are the per-query Arrow pool peaks that `--explain-analyze` prints.",
            "",
            f"Source: `{path}`.",
        ]
    )


def render_batch(root):
    found = newest(root, "ablation")
    if not found:
        return NO_RESULT
    path, d = found
    sizes = sorted({int(c["config"].split("=")[1]) for c in d["cells"] if c["group"] == "batch"})
    rows = []
    for q in MAIN_QUERIES:
        rows.append(
            [QUERY_LABEL[q]]
            + [stat(pick(d["cells"], group="batch", query=q, config=f"batch_size={s}")) for s in sizes]
        )
    return "\n".join(
        [table(["Query"] + [f"batch {s}" for s in sizes], rows), "", source_line(path, d), flagged(d)]
    )


def render_pruning(root):
    found = newest(root, "ablation")
    if not found:
        return NO_RESULT
    path, d = found
    rows = []
    for dataset, label in (("sorted", "sorted by l_shipdate"), ("default", "dbgen order")):
        for q in ("q1", "q6"):
            on = pick(d["cells"], group="pruning", query=q, dataset=dataset, engine="quarry", config="default")
            off = pick(
                d["cells"], group="pruning", query=q, dataset=dataset, engine="quarry",
                config="without prune_row_groups",
            )
            duck = pick(d["cells"], group="pruning", query=q, dataset=dataset, engine="duckdb")
            rows.append([label, QUERY_LABEL[q], stat(on), stat(off), stat(duck) if duck else "not run"])
    header = ["lineitem", "Query", "quarry", "quarry, prune_row_groups off", "DuckDB threads=1"]
    return "\n".join([table(header, rows), "", source_line(path, d), flagged(d)])


def render_rules(root):
    found = newest(root, "ablation")
    if not found:
        return NO_RESULT
    path, d = found
    rows = []
    for rule, queries in (("prune_columns", MAIN_QUERIES), ("order_joins", ["q3", "q5"])):
        for q in queries:
            on = pick(d["cells"], group="rules", query=q, config="default")
            off = pick(d["cells"], group="rules", query=q, config=f"without {rule}")
            rows.append([f"`{rule}`", QUERY_LABEL[q], stat(on), stat(off)])
    return "\n".join(
        [table(["Rule switched off", "Query", "quarry, all rules", "quarry, rule off"], rows), "",
         source_line(path, d), flagged(d)]
    )


def render_spill(root):
    found = newest(root, "spill")
    if not found:
        return NO_RESULT
    path, d = found
    budgets = ["unlimited", "256MB", "64MB", "16MB"]
    rows = []
    for q in ("q3", "q5", "agg"):
        for b in budgets:
            c = pick(d["cells"], group="spill", query=q, config=b)
            if c is None:
                continue
            if c["status"] != "ok":
                shown = f"refused: {c['error']}" if c["status"] == "error" else c["status"]
                rows.append([QUERY_LABEL[q], b, shown, "-", "-", "-"])
            else:
                rows.append(
                    [QUERY_LABEL[q], b, stat(c), mib(c["spilled_bytes"]), mib(c["pool_peak_bytes"]), mib(c["peak_rss_bytes"])]
                )
    header = ["Query", "Memory limit", "Time (ms)", "Spilled (MiB)", "Operator state peak (MiB)", "Peak RSS (MiB)"]
    return "\n".join(
        [
            table(header, rows),
            "",
            "The limit bounds operator state only, not the Parquet reader's row-group buffers, so peak RSS "
            "stays above it. Batches read back from spill files use Arrow's default pool and are outside the "
            "operator-state peak (docs/design/0006-memory-and-spill.md). A refused cell is the result of that cell.",
            "",
            source_line(path, d),
            flagged(d),
        ]
    )


PROFILE_DIR = RESULTS / "profile"
PROFILE_NAMES = [
    ("q1", "Q1"), ("q3", "Q3"), ("q5", "Q5"), ("q5-without-order_joins", "Q5, `order_joins` off"), ("q6", "Q6"),
]
OPERATOR_LINE = re.compile(r"^\s*(\w+) #(\d+) .*? rows=(\d+) .*?time=([\d.]+)ms peak=(\S+)")


def render_profile_categories(root):
    path = root / PROFILE_DIR / "classified.json"
    if not path.exists():
        return NO_RESULT
    data = json.loads(path.read_text(encoding="utf-8"))
    categories = ["Parquet decode", "filter", "scan other", "hash build", "probe", "aggregate", "sort", "other"]
    rows = []
    for name, label in PROFILE_NAMES:
        if name in data:
            rows.append([label] + [f"{data[name]['share'][c] * 100:.0f}%" for c in categories] + [f"{data[name]['total']:,}"])
    return "\n".join(
        [
            table(["Query"] + categories + ["samples"], rows),
            "",
            "Share of the samples taken on non-idle threads (`sample <pid> 10`, 1 ms interval) while "
            "`quarry query --repeat` looped the query on SF1; `bench/classify_samples.py` explains the categories. "
            f"Source: `{PROFILE_DIR.as_posix()}/classified.json`.",
        ]
    )


def render_profile_frames(root):
    path = root / PROFILE_DIR / "classified.json"
    if not path.exists():
        return NO_RESULT
    data = json.loads(path.read_text(encoding="utf-8"))
    rows = []
    for name, label in PROFILE_NAMES:
        if name in data:
            for t in data[name]["top_frames"][:5]:
                rows.append([label, f"`{t['frame']}`", t["category"], f"{t['share'] * 100:.1f}%"])
    return table(["Query", "Function on top of the stack", "Category", "Share of samples"], rows)


def render_profile_operators(root):
    rows = []
    for name, label in PROFILE_NAMES:
        path = root / PROFILE_DIR / f"explain-analyze-{name}.txt"
        if not path.exists():
            continue
        operators = []
        for line in path.read_text(encoding="utf-8").splitlines():
            m = OPERATOR_LINE.match(line)
            if m:
                operators.append((float(m.group(4)), m.group(1), m.group(2), int(m.group(3)), m.group(5)))
        total = sum(o[0] for o in operators)
        for time_ms, kind, number, out_rows, peak in sorted(operators, reverse=True)[:3]:
            rows.append([label, f"{kind} #{number}", ms(time_ms), f"{time_ms / total * 100:.0f}%", f"{out_rows:,}", peak])
    if not rows:
        return NO_RESULT
    return "\n".join(
        [
            table(["Query", "Operator", "Own time (ms)", "Share of operator time", "Rows out", "Peak state"], rows),
            "",
            f"The three operators with the most own time in each `--explain-analyze` run on SF1 (`{PROFILE_DIR.as_posix()}/explain-analyze-*.txt`).",
        ]
    )


def render_gaps(root):
    found = newest(root, "main")
    if not found:
        return NO_RESULT
    path, d = found
    rows = []
    for q in MAIN_QUERIES:
        quarry = pick(d["cells"], engine="quarry", query=q)
        one = pick(d["cells"], engine="duckdb", config="threads=1", query=q)
        rows.append((quarry["median_ms"] - one["median_ms"], q, quarry["median_ms"], one["median_ms"]))
    body = [
        [QUERY_LABEL[q], ms(a), ms(b), ms(gap), f"{a / b:.2f}"] for gap, q, a, b in sorted(rows, reverse=True)
    ]
    return "\n".join(
        [
            table(["Query", "quarry (ms)", "DuckDB threads=1 (ms)", "Gap (ms)", "Ratio"], body),
            "",
            f"Medians from `{path}`, largest gap first.",
        ]
    )


BLOCKS = {
    "gaps": render_gaps,
    "profile-categories": render_profile_categories,
    "profile-frames": render_profile_frames,
    "profile-operators": render_profile_operators,
    "environment": render_environment,
    "main": render_main,
    "memory": render_memory,
    "ablation-batch": render_batch,
    "ablation-pruning": render_pruning,
    "ablation-rules": render_rules,
    "spill": render_spill,
}
MARKER = re.compile(r"(<!-- bench:([a-z-]+) -->\n)(.*?)\n?(<!-- /bench:\2 -->)", re.DOTALL)


def render_file(text, root, name):
    def replace(match):
        block = match.group(2)
        if block not in BLOCKS:
            raise KeyError(f"{name}: unknown table '{block}'; known: {', '.join(sorted(BLOCKS))}")
        return match.group(1) + BLOCKS[block](root) + "\n" + match.group(4)

    return MARKER.sub(replace, text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="change nothing; fail when a table differs")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    root = args.repo_root.resolve()

    status = 0
    for name in DOCS:
        path = root / name
        if not path.exists():
            continue
        old = path.read_text(encoding="utf-8")
        try:
            new = render_file(old, root, name)
        except KeyError as error:
            print(f"render_tables: {error.args[0]}", file=sys.stderr)
            return 1
        if new == old:
            continue
        if args.check:
            status = 1
            sys.stdout.writelines(
                difflib.unified_diff(
                    old.splitlines(True), new.splitlines(True), f"{name} (committed)", f"{name} (rendered)"
                )
            )
        else:
            path.write_text(new, encoding="utf-8")
            print(f"rendered {name}")
    if args.check and status:
        print("render_tables: the tables differ from the committed results; run bench/render_tables.py", file=sys.stderr)
    return status


if __name__ == "__main__":
    sys.exit(main())
