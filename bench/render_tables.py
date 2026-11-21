#!/usr/bin/env python3
"""Renders the benchmark tables of the README and the docs from the committed results.

  uv run bench/render_tables.py            rewrite the tables in place
  uv run bench/render_tables.py --check    change nothing; exit 1 and show a diff when a table differs

A table lives between `<!-- bench:NAME -->` and `<!-- /bench:NAME -->`; everything between the two
is replaced. A number in prose is written `<!-- n:NAME -->value<!-- /n -->`; the value is replaced
by what the results say (build_facts), and --check fails on one that no result produces. Each table is rendered from the newest result file of one suite in bench/results/
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
def doc_files(root):
    """Every document whose numbers are checked: the README and everything under docs/ (the design
    records and the operations guide included)."""
    found = [root / "README.md"] + sorted((root / "docs").glob("**/*.md"))
    return [p.relative_to(root).as_posix() for p in found if p.exists()]


# A number with a unit that reads as a measurement or a size. A paragraph that holds one must be
# bound to a result (a `bench:` block or an `n:` marker), or carry `<!-- obs: where it came from -->`
# (an observation that no committed result holds) or `<!-- const -->` (a value the code or the
# protocol stipulates, not a measurement).
MEASUREMENT = re.compile(r"(?<![\w.])\d[\d,]*(?:\.\d+)?\s?(?:ms|MiB|GiB|GB|KiB|MB|x|%|seconds?|s)(?!\w)")
LABEL = re.compile(r"<!--\s*(obs|const)\b")
QUERY_LABEL = {"q1": "Q1", "q3": "Q3", "q5": "Q5", "q6": "Q6", "agg": "High-cardinality aggregate"}
MAIN_QUERIES = ["q1", "q3", "q5", "q6"]
NO_RESULT = "_No result is committed for this table yet._"


def newest(root, suite, directory=RESULTS):
    """The newest usable result of a suite: (relative path, document), or None."""
    found = []
    for path in sorted((root / directory).glob(f"{suite}-*.json")):
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


def mb(value):
    # Decimal megabytes (10^6 bytes), the unit of --memory-limit, with two decimals so that a peak just
    # above its limit does not print like the limit.
    return f"{value / 1e6:,.2f}"


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


def flagged(document, group):
    """The load-average sentence under a table: the flagged cells of that table's group only, each
    with its dataset. A cell with a null `load_flag` never ran a measurement and is not flagged."""
    cells = [c for c in document["cells"] if c["group"] == group]
    names = [
        f"{c['engine']} {c['config']} {QUERY_LABEL[c['query']]} ({c['dataset']} data)"
        for c in cells
        if c.get("load_flag") is True
    ]
    if names:
        text = "Cells that ran with a 1-minute load average above 1.5 after the wait: " + ", ".join(names) + "."
    else:
        text = "The 1-minute load average stayed at or below 1.5 before every run of every measured cell."
    if any(c.get("load_flag") is None for c in cells):
        text += " A cell that timed out or crashed has no load figure."
    return text


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
            flagged(d, "engines"),
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
        "Query", "quarry peak RSS", "quarry operator pool peak", "quarry scan pool peak",
        "DuckDB threads=1 peak RSS", "DuckDB default peak RSS", "Python + DuckDB idle RSS",
    ]
    return "\n".join(
        [
            table(header, rows),
            "",
            "MiB. The DuckDB peaks include the Python interpreter, `duckdb` and `pyarrow`; the idle column is "
            "that process's RSS just before its first query and is not subtracted. quarry's two pool peaks are the "
            "per-query Arrow pool peaks that `--explain-analyze` prints. The operator pool holds the hash tables' "
            "storage (key index, key arena, aggregate states, join chains), sort and build-row buffers, spill "
            "buffers and expression temporaries; the scan pool holds the Parquet reader's row-group buffers. "
            "Neither includes the result table.",
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
        [table(["Query"] + [f"batch {s}" for s in sizes], rows), "", source_line(path, d), flagged(d, "batch")]
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
    return "\n".join([table(header, rows), "", source_line(path, d), flagged(d, "pruning")])


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
         source_line(path, d), flagged(d, "rules")]
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
                    [QUERY_LABEL[q], b, stat(c), mb(c["spilled_bytes"]), mb(c["pool_peak_bytes"]), mb(c["peak_rss_bytes"])]
                )
    header = ["Query", "Memory limit (MB)", "Time (ms)", "Spilled (MB)", "Operator pool peak (MB)", "Peak RSS (MB)"]
    return "\n".join(
        [
            table(header, rows),
            "",
            "The limit bounds operator state, not the Parquet reader's row-group buffers, so peak RSS stays above "
            "it. The operator pool peak is what the query's operators allocated through Arrow's memory pool: the "
            "hash tables' storage (key index, key arena, aggregate states, join chains), sort and build-row "
            "buffers, spill buffers, batches read back from spill files and expression temporaries. It excludes "
            "the Parquet reader's row-group buffers (their own pool), the result table, and the strings that "
            "MIN and MAX of a VARCHAR hold (docs/design/0006-memory-and-spill.md). The limit and the three sizes "
            "are in the same unit, MB of 10^6 bytes, as `--memory-limit` counts them. A join's build rows are "
            "charged once, but a join makes a second copy of them when it concatenates its build side, and that "
            "copy can sit in the operator pool, so the pool peak can pass the limit: Q5 at 16MB is above it "
            "(known issue in the CHANGELOG). A refused cell is the result of that cell.",
            "",
            source_line(path, d),
            flagged(d, "spill"),
        ]
    )


BEFORE_FIXES = RESULTS / "before-review-fixes"


def render_fix_wave(root):
    """The four queries before and after the fixes of the whole-branch review: quarry's median and its
    ratio to single-threaded DuckDB in each of the two main results. It renders no spill cells; the
    spill table has its own renderer."""
    new = newest(root, "main")
    old = newest(root, "main", BEFORE_FIXES)
    if not new or not old:
        return NO_RESULT
    rows = []
    for q in MAIN_QUERIES:
        cells = []
        for _, document in (old, new):
            quarry = pick(document["cells"], engine="quarry", query=q)
            one = pick(document["cells"], engine="duckdb", config="threads=1", query=q)
            cells.append((quarry, one))
        (q_old, d_old), (q_new, d_new) = cells
        if any(c["status"] != "ok" for c in (q_old, d_old, q_new, d_new)):
            rows.append([QUERY_LABEL[q]] + ["-"] * 5)
            continue
        rows.append(
            [
                QUERY_LABEL[q], ms(q_old["median_ms"]), ms(q_new["median_ms"]),
                f"{q_old['median_ms'] / d_old['median_ms']:.2f}", f"{q_new['median_ms'] / d_new['median_ms']:.2f}",
                f"{(q_new['median_ms'] / q_old['median_ms'] - 1) * 100:+.1f}%",
            ]
        )
    header = [
        "Query", "quarry before (ms)", "quarry after (ms)", "ratio before", "ratio after", "quarry time change",
    ]
    return "\n".join(
        [
            table(header, rows),
            "",
            f"Before: `{old[0]}` (commit `{old[1]['repo_commit'][:12]}`). After: `{new[0]}` "
            f"(commit `{new[1]['repo_commit'][:12]}`). The ratio is quarry's median over DuckDB's, "
            "`threads=1`, in the same result file.",
        ]
    )


PROFILE_DIR = RESULTS / "profile"
BEFORE_DIR = PROFILE_DIR / "before-join-estimate-fix"
FIXES_DIR = PROFILE_DIR / "before-review-fixes"
# (directory, file name stem, label). The first Q5 is the plan that ships; the last Q5 profile is
# the one taken before the join-estimate fix, kept and labelled as such.
PROFILE_SOURCES = [
    (PROFILE_DIR, "q1", "Q1"), (PROFILE_DIR, "q3", "Q3"), (PROFILE_DIR, "q5", "Q5"),
    (PROFILE_DIR, "q5-without-order_joins", "Q5, `order_joins` off"), (PROFILE_DIR, "q6", "Q6"),
    (BEFORE_DIR, "q5", "Q5, before the join-estimate fix"),
    (FIXES_DIR, "q1", "Q1, before the fixes of the whole-branch review"),
    (FIXES_DIR, "q5", "Q5, before the fixes of the whole-branch review"),
]
OPERATOR_LINE = re.compile(r"^\s*(\w+) #(\d+) .*? rows=(\d+) .*?time=([\d.]+)ms peak=(\S+)")


def classified(root, directory):
    path = root / directory / "classified.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}


def render_profile_categories(root):
    categories = ["Parquet decode", "filter", "scan other", "hash build", "probe", "aggregate", "sort", "other"]
    rows = []
    for directory, name, label in PROFILE_SOURCES:
        data = classified(root, directory)
        if name in data:
            rows.append([label] + [f"{data[name]['share'][c] * 100:.0f}%" for c in categories] + [f"{data[name]['total']:,}"])
    if not rows:
        return NO_RESULT
    return "\n".join(
        [
            table(["Query"] + categories + ["samples"], rows),
            "",
            "Share of the samples taken on non-idle threads (`sample <pid> 10`, 1 ms interval) while "
            "`quarry query --repeat` looped the query on SF1; `bench/classify_samples.py` explains the categories. "
            f"Source: `{PROFILE_DIR.as_posix()}/classified.json` (the row before the fix: `{BEFORE_DIR.as_posix()}/classified.json`).",
        ]
    )


def render_profile_frames(root):
    rows = []
    for directory, name, label in PROFILE_SOURCES:
        data = classified(root, directory)
        if name in data:
            for t in data[name]["top_frames"][:5]:
                rows.append([label, f"`{t['frame']}`", t["category"], f"{t['share'] * 100:.1f}%"])
    return table(["Query", "Function on top of the stack", "Category", "Share of samples"], rows)


def render_profile_operators(root):
    rows = []
    for directory, name, label in PROFILE_SOURCES:
        path = root / directory / f"explain-analyze-{name}.txt"
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


# Numbers quoted in prose. A prose number is written `<!-- n:NAME -->value<!-- /n -->`; the renderer
# replaces the value with what the committed results say, so --check fails on a prose number that
# no result produced, as it does for a table.
BEFORE_RESULTS = RESULTS / "before-join-estimate-fix"
PROFILE_TAGS = {
    "q1": (PROFILE_DIR, "q1"), "q3": (PROFILE_DIR, "q3"), "q5": (PROFILE_DIR, "q5"),
    "q5off": (PROFILE_DIR, "q5-without-order_joins"), "q6": (PROFILE_DIR, "q6"),
    "q5before": (BEFORE_DIR, "q5"),
    "q1fixes": (FIXES_DIR, "q1"), "q5fixes": (FIXES_DIR, "q5"),
}


def comma(value):
    return f"{value:,}"


def add_main_facts(facts, prefix, document):
    for q in MAIN_QUERIES:
        quarry = pick(document["cells"], engine="quarry", query=q)
        one = pick(document["cells"], engine="duckdb", config="threads=1", query=q)
        if quarry["status"] == "ok":
            facts[f"{prefix}{q}.quarry"] = ms(quarry["median_ms"])
            facts[f"{prefix}{q}.rss"] = mib(quarry["peak_rss_bytes"])
            facts[f"{prefix}{q}.state"] = mib(quarry["pool_peak_bytes"])
        if one["status"] == "ok":
            facts[f"{prefix}{q}.duckdb"] = ms(one["median_ms"])
        if quarry["status"] == "ok" and one["status"] == "ok":
            facts[f"{prefix}{q}.ratio"] = f"{quarry['median_ms'] / one['median_ms']:.2f}"
            facts[f"{prefix}{q}.gap"] = ms(quarry["median_ms"] - one["median_ms"])


def build_facts(root):
    facts = {}
    found = newest(root, "main")
    if found:
        add_main_facts(facts, "main.", found[1])
        rows = found[1].get("data", {}).get("default", {}).get("lineitem_rows")
        if rows is not None:
            facts["data.lineitem_rows"] = comma(rows)
    found = newest(root, "main", BEFORE_RESULTS)
    if found:
        add_main_facts(facts, "before.main.", found[1])
    found = newest(root, "main", BEFORE_FIXES)
    if found:
        add_main_facts(facts, "fixes.before.main.", found[1])
    found = newest(root, "ablation")
    if found:
        cells = found[1]["cells"]
        for c in cells:
            if c["group"] == "batch" and c["status"] == "ok":
                facts[f"batch.{c['config'].split('=')[1]}.{c['query']}"] = ms(c["median_ms"])
            if c["group"] == "rules" and c["status"] == "ok":
                if c["config"] == "default":
                    facts[f"rules.on.{c['query']}"] = ms(c["median_ms"])
                else:
                    facts[f"rules.{c['config'].split()[-1]}.off.{c['query']}"] = ms(c["median_ms"])
    found = newest(root, "ablation", BEFORE_RESULTS)
    if found:
        for c in found[1]["cells"]:
            if c["group"] == "rules" and c["status"] == "ok" and c["config"] == "without order_joins":
                facts[f"before.rules.order_joins.off.{c['query']}"] = ms(c["median_ms"])
    found = newest(root, "spill")
    if found:
        for c in found[1]["cells"]:
            if c["status"] == "ok":
                facts[f"spill.{c['query']}.{c['config']}.ms"] = ms(c["median_ms"])
                facts[f"spill.{c['query']}.{c['config']}.spilled"] = mb(c["spilled_bytes"])
                facts[f"spill.{c['query']}.{c['config']}.state"] = mb(c["pool_peak_bytes"])
    found = newest(root, "spill", BEFORE_RESULTS)
    if found:
        for c in found[1]["cells"]:
            if c["status"] == "ok":
                facts[f"before.spill.{c['query']}.{c['config']}.ms"] = ms(c["median_ms"])
    found = newest(root, "spill", BEFORE_FIXES)
    if found:
        for c in found[1]["cells"]:
            if c["status"] == "ok":
                facts[f"fixes.before.spill.{c['query']}.{c['config']}.ms"] = ms(c["median_ms"])
                facts[f"fixes.before.spill.{c['query']}.{c['config']}.state"] = mb(c["pool_peak_bytes"])
                facts[f"fixes.before.spill.{c['query']}.{c['config']}.spilled"] = mb(c["spilled_bytes"])
    for tag, (directory, name) in PROFILE_TAGS.items():
        path = root / directory / f"explain-analyze-{name}.txt"
        if path.exists():
            lines = path.read_text(encoding="utf-8").splitlines()
            matched = [OPERATOR_LINE.match(line) for line in lines]
            total = sum(float(m.group(4)) for m in matched if m) or 1.0
            for line, m in zip(lines, matched):
                if m:
                    key = f"ea.{tag}.{m.group(2)}"
                    facts[f"{key}.share"] = f"{float(m.group(4)) / total * 100:.0f}"
                    facts[f"{key}.rows"] = comma(int(m.group(3)))
                    facts[f"{key}.time"] = ms(float(m.group(4)))
                    facts[f"{key}.peak"] = m.group(5)
                est = re.search(r"#(\d+) .*? est=(\d+) rows=(\d+)", line)
                if est:
                    facts[f"ea.{tag}.{est.group(1)}.est"] = comma(int(est.group(2)))
        data = classified(root, directory).get(name)
        if data:
            facts[f"cls.{tag}.samples"] = comma(data["total"])
            for category, share in data["share"].items():
                facts[f"cls.{tag}.{category}"] = f"{share * 100:.0f}"
            seen = set()
            for t in data["top_frames"]:
                function = t["frame"].replace("quarry::", "").replace("eval_internal::", "")
                if (function, t["category"]) not in seen:
                    seen.add((function, t["category"]))
                    facts[f"frame.{tag}.{function}.{t['category']}"] = f"{t['share'] * 100:.1f}"
    return facts


FACT = re.compile(r"<!-- n:([A-Za-z0-9_.:+$ -]+?) -->(.*?)<!-- /n -->")


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
    "fix-wave": render_fix_wave,
}
MARKER = re.compile(r"(<!-- bench:([a-z-]+) -->\n)(.*?)\n?(<!-- /bench:\2 -->)", re.DOTALL)


def render_file(text, root, name):
    def replace(match):
        block = match.group(2)
        if block not in BLOCKS:
            raise KeyError(f"{name}: unknown table '{block}'; known: {', '.join(sorted(BLOCKS))}")
        return match.group(1) + BLOCKS[block](root) + "\n" + match.group(4)

    text = MARKER.sub(replace, text)
    facts = build_facts(root)

    def substitute(match):
        if match.group(1) not in facts:
            raise KeyError(f"{name}: no result produces the prose number '{match.group(1)}'")
        return f"<!-- n:{match.group(1)} -->{facts[match.group(1)]}<!-- /n -->"

    return FACT.sub(substitute, text)


def unlabelled_numbers(text):
    """(line number, line) of every line that has a measurement-like number and is in a paragraph with
    no label, outside code fences, `bench:` blocks and the values of `n:` markers."""
    found = []
    paragraph = []
    fenced = False
    in_block = False

    def flush():
        if paragraph and not any(LABEL.search(line) for _, line in paragraph):
            for number, line in paragraph:
                plain = FACT.sub("N", line)
                plain = re.sub(r"`[^`]*`", "", plain)
                plain = re.sub(r"<!--.*?-->", "", plain)
                if MEASUREMENT.search(plain):
                    found.append((number, line.strip()))
        paragraph.clear()

    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("```"):
            flush()
            fenced = not fenced
            continue
        if fenced:
            continue
        if line.startswith("<!-- bench:"):
            flush()
            in_block = True
            continue
        if line.startswith("<!-- /bench:"):
            in_block = False
            continue
        if in_block:
            continue
        if not line.strip():
            flush()
            continue
        paragraph.append((number, line))
    flush()
    return found


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--check", action="store_true", help="change nothing; fail when a table differs")
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    args = parser.parse_args(argv)
    root = args.repo_root.resolve()

    status = 0
    for name in doc_files(root):
        path = root / name
        old = path.read_text(encoding="utf-8")
        try:
            new = render_file(old, root, name)
        except KeyError as error:
            print(f"render_tables: {error.args[0]}", file=sys.stderr)
            return 1
        for number, line in unlabelled_numbers(new):
            status = 1
            print(
                f"render_tables: {name}:{number}: a number with a unit that no result produces and no "
                f"label explains: {line[:100]}",
                file=sys.stderr,
            )
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
        print(
            "render_tables: a table differs from the committed results (run bench/render_tables.py) or a "
            "number is unlabelled",
            file=sys.stderr,
        )
    return status


if __name__ == "__main__":
    sys.exit(main())
