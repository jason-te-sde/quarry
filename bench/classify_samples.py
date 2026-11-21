#!/usr/bin/env python3
"""Classifies macOS `sample` call graphs of a looping quarry run into operator-level categories.

  uv run bench/classify_samples.py

Reads bench/results/profile/callgraph-<name>.txt.gz (the "Call graph" section of `sample <pid> 10`
taken while `quarry query --repeat N` looped one query on the SF1 data) and writes
bench/results/profile/classified.json: for each name, the samples per category as a count and as a
share of the samples taken on threads that were not idle.

Every sample is attributed by walking from the sampled function up its call stack to the nearest
frame that names an owner. In a pull-based engine an operator's frame contains its children's, so
the nearest owner is the code that did the work:

  Parquet decode   under a Scan, below any of parquet::, snappy::, arrow:: or pread, and not below
                   a predicate evaluation
  filter           a predicate evaluation (EvaluatePredicate, in a Scan or a Filter operator)
  scan other       the rest of a Scan: row-group setup, selection gather, batch assembly
  hash build       HashJoinOperator::IngestBuildBatch / ConcatenateBuild and JoinHashTable::Insert,
                   including the key index
  probe            JoinHashTable::Lookup and the rest of HashJoinOperator (output assembly)
  aggregate        HashAggregateOperator and AggHashTable, including evaluating the aggregate
                   arguments
  sort             Sort and Top-N operators
  other            everything else (Project, the CLI, the allocator outside an owner)

A thread blocked in a condition-variable wait (the Arrow thread pool's idle workers) is not counted.
"""

import gzip
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PROFILE_DIR = ROOT / "bench" / "results" / "profile"
CATEGORIES = ["Parquet decode", "filter", "scan other", "hash build", "probe", "aggregate", "sort", "other"]
IDLE = ("__psynch_cvwait", "__psynch_mutexwait", "_pthread_cond_wait", "__semwait_signal", "kevent")
LINE = re.compile(r"^([ +!:|]*?)(\d+) (.*)$")
DECODE = re.compile(r"parquet::|snappy::|^arrow::|pread|zstd|ZSTD")


def parse(text):
    """Yields (stack of function names root-first, self sample count) for every node of the tree."""
    stack = []  # (column, name, count, children_count_ref)
    nodes = []
    for line in text.splitlines():
        match = LINE.match(line)
        if not match:
            continue
        column, count, name = len(match.group(1)), int(match.group(2)), match.group(3)
        while stack and stack[-1][0] >= column:
            stack.pop()
        node = {"name": name, "count": count, "children": 0, "path": [n["name"] for _, n in stack]}
        if stack:
            stack[-1][1]["children"] += count
        stack.append((column, node))
        nodes.append(node)
    for node in nodes:
        own = node["count"] - node["children"]
        if own > 0:
            yield node["path"] + [node["name"]], own


def classify(path):
    leaf = path[-1]
    if leaf.startswith(IDLE):
        return None
    below_predicate = False
    below_decode = False
    for name in reversed(path):
        if "EvaluatePredicate" in name:
            below_predicate = True
        elif DECODE.search(name):
            below_decode = True
        if "ScanOperator" in name:
            if below_predicate:
                return "filter"
            return "Parquet decode" if below_decode else "scan other"
        if re.search(r"FilterOperator", name):
            return "filter"
        if re.search(r"IngestBuildBatch|ConcatenateBuild|JoinHashTable::Insert", name):
            return "hash build"
        if re.search(r"JoinHashTable::Lookup|HashJoinOperator", name):
            return "probe"
        if re.search(r"HashAggregateOperator|AggHashTable", name):
            return "aggregate"
        if re.search(r"SortOperator|TopNOperator", name):
            return "sort"
    return "other"


def short_name(frame):
    """`quarry::Gather(arrow::Array const&, ...)  (in quarry) + 12  [0x...]` -> `quarry::Gather`."""
    name = re.sub(r"\s+\(in [^)]*\).*$", "", frame).replace("(anonymous namespace)::", "")
    name = name.split("(")[0].strip()
    previous = None
    while previous != name:  # drop template arguments, innermost first
        previous = name
        name = re.sub(r"<[^<>]*>", "", name)
    return name


def classify_file(path):
    counts = dict.fromkeys(CATEGORIES, 0)
    leaves = {}
    with gzip.open(path, "rt", encoding="utf-8") as handle:
        for stack, own in parse(handle.read()):
            category = classify(stack)
            if category is not None:
                counts[category] += own
                key = (category, short_name(stack[-1]))
                leaves[key] = leaves.get(key, 0) + own
    total = sum(counts.values())
    top = sorted(leaves.items(), key=lambda item: -item[1])[:8]
    return {
        "top_frames": [
            {"category": c, "frame": f, "samples": n, "share": round(n / total, 4)} for (c, f), n in top
        ],
        "samples": counts,
        "total": total,
        "share": {k: round(v / total, 4) if total else 0.0 for k, v in counts.items()},
    }


def main():
    result = {}
    for path in sorted(PROFILE_DIR.glob("callgraph-*.txt.gz")):
        result[path.name[len("callgraph-") : -len(".txt.gz")]] = classify_file(path)
    if not result:
        print("classify_samples: no call graphs in bench/results/profile/", file=sys.stderr)
        return 2
    (PROFILE_DIR / "classified.json").write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    for name, entry in result.items():
        shares = ", ".join(f"{k} {v * 100:.0f}%" for k, v in entry["share"].items() if v)
        print(f"{name}: {entry['total']} samples: {shares}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
