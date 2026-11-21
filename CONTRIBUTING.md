# Contributing to quarry

Thank you for looking. quarry is small on purpose and is held to a high standard of proof, so a change
is welcome when it comes with the test that shows it is right. This page is the shortest route to a
change that gets merged. To report a security problem, read [SECURITY.md](SECURITY.md) first and do not
open a public issue.

## Before you start

- **Read** the [README](README.md), [docs/architecture.md](docs/architecture.md) and
  [docs/testing.md](docs/testing.md). The design records in [docs/design](docs/design/) say why the big
  decisions are what they are, and [0007](docs/design/0007-scope.md) lists what is deliberately not
  built. A change that adds something on that list (an outer join, `DISTINCT`, a thread pool, ...) needs
  a conversation first: open an issue.
- **Open an issue** for anything bigger than a typo, so that the shape of the change can be agreed
  before the work is done.
- A bug report is best with a **seed** or a query. If the differential test found it, give the seed and
  the command that repeats it (`uv run tests/differential/run.py --quarry <quarry> --seed N`), and the
  toolchain. If something else found it, give the SQL and the smallest Parquet file that shows it.

## Building and testing

You need a C++20 compiler (GCC 14, Clang 18 or Apple Clang 15 or newer), CMake 3.25, Ninja, and
[uv](https://docs.astral.sh/uv/) for the Python tools. Arrow and Parquet are not fetched by the build:
build them once with `scripts/build_arrow.sh`, which downloads the pinned source release, verifies its
checksum and installs it under `~/.local/opt`. GoogleTest is fetched at a pinned tag.

```
scripts/build_arrow.sh
cmake --preset dev && cmake --build --preset dev
ctest --preset dev
```

The other presets are `release`, `asan`, `coverage` and `fuzz`. [docs/testing.md](docs/testing.md) says
what each layer of tests is for and how to run it. Before you push, run **`scripts/preflight.sh`**: it
does what CI does, in CI's order, and fails if a tool it needs is missing. It takes a long time, which is
cheaper than a red build. At the least, run `ctest --preset dev`, `scripts/check-layering.sh` and
`scripts/check-hygiene.sh`.

## The rules

Most of these rules are checked by the compiler, by a script or by a test, and a change that breaks one of
those is refused by CI whatever else it does. The rules marked **held by review** are not checked by any
tool: a reviewer holds you to them.

- **C++20, no extensions.**
- **Warnings are errors**: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror`, on every target of quarry including tests and tools.
- **Errors are values.** A function that can fail returns an `arrow::Status` or an `arrow::Result`.
  **Nothing under `src/` or `include/` throws**, and **library code writes nothing to standard output or
  error**: `scripts/check-layering.sh` fails on both. Tools under `tools/` may print.
- **Layering** ([architecture](docs/architecture.md#layers)): the include direction between the
  directories of `src/` follows the link graph of `src/CMakeLists.txt`, and
  `scripts/check-layering.sh` reads every `#include`. `src/parser` includes no Arrow header.
- **No `arrow::compute`.** quarry computes with its own kernels, so that the engine is what the
  benchmarks time ([ADR 0003](docs/design/0003-arrow-boundary.md)). The layering script bans the include
  and the namespace in source, and `scripts/check-objects.sh` looks for the symbols in the compiled
  objects.
- **An invariant is enforced by the class, not by a comment.** The memory budget refuses a request over
  the limit, the spill directory removes itself, a selection vector checks its indices. A rule that only a
  comment states will be broken (held by review).
- **Formatting** is `clang-format` with the repository's `.clang-format` (Google style, 100 columns) at
  the major version CI pins, and **`clang-tidy`** with the repository's `.clang-tidy` and warnings as
  errors, on `src/` and `tools/`.
- **Everything in the repository is English**: code, comments, documents, commit messages, test names.
  `scripts/check-hygiene.sh` checks that no tracked file and no commit message contains Chinese,
  Japanese or Korean characters, an authorship credit, or an absolute path of a home directory.
- **Numbers in the README and the docs come from `bench/results/`.** A table is rendered by
  `uv run bench/render_tables.py`, and a number in prose is written as
  `<!-- n:NAME -->value<!-- /n -->` so that it is rendered too. `scripts/check-readme-numbers.sh` fails
  when one disagrees. A result is produced by `bench/run_bench.py` on a quiet machine and by the protocol
  in [docs/benchmarks.md](docs/benchmarks.md), which refuses a tree with uncommitted changes.

## Tests come first

1. **Write the test that fails**, run it, and see it fail for the reason you expect.
2. Change the code until it passes.
3. Keep the test.

A fix with no test that fails without it is not finished. Put the test in the layer that can catch the
bug most cheaply: a unit test for a kernel, a rule or an operator; an entry in `tests/sql/` for a
message, a position or a result; the differential test for something about queries in general. If the
differential test found the bug, add its seed to `tests/differential/regression_seeds.txt` as well. A
change to what the optimizer chooses updates the golden plans in `tests/planner/plans/` (`UPDATE_GOLDENS=1`
rewrites them locally and is refused in CI), and the diff of a golden file is what a
reviewer reads.

A change to the accepted SQL changes [docs/sql.md](docs/sql.md) in the same commit, and the table of
DuckDB's types in it is checked against the code by `TypeRulesTest.DocsTableMatchesTheRules`. The same
goes for a default or a limit in [docs/operations.md](docs/operations.md).

## Commits

Small, one logical change each, each building and passing its own tests. The subject starts with a type
(`feat`, `fix`, `test`, `docs`, `build`, `ci`, `style`, `chore`) or the area it touches (`parser`,
`binder`, `planner`, `exec`, `expr`, `io`, `bench`), then a colon and what changed. The body is where the
reasoning goes, and is not optional for anything but a trivial change:

- **Why**: what was wrong or missing, and how you know.
- **What**: what the change does.
- **Considered and rejected**: the alternatives and why not.
- **Not done**: what this leaves for later, honestly.
- **Verified**: what you ran, and what it showed, including the test that failed before the change.

## Pull requests

- Keep it small enough to review. Say what you ran.
- CI must be green. It runs the compiler matrix, the sanitizers, the injected flaws, the fuzzers, the
  differential test, the lint checks, coverage, the benchmark harness smoke test and a clean-clone build
  that follows the README; see [docs/testing.md](docs/testing.md).
- Pull requests are merged with a merge commit once the branch is green, so write each commit as it
  should read in the history.

## What gets a change refused

- It weakens a check to make something pass: a looser comparator, a test that cannot fail, a floor
  lowered.
- It adds a dependency, a global, a throw, or output from library code.
- It claims something the tests or the measurements do not show. The documents are held to the same rule
  as the code: if a sentence says the engine does something, there is a test, a commit or a measurement
  behind it.
