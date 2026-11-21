#!/usr/bin/env bash
# scripts/preflight.sh: what CI runs, locally, in CI's order.
#
# Run it before pushing. A stage that cannot run, because a tool is missing or has the wrong
# version, fails the whole script: a preflight that quietly checks half of CI produces the
# confidence without the coverage.
#
# What it does not run, which is a gap of this loop and not of what CI covers: the coverage job,
# which needs the gcovr that the job pins; the compilers of the build-test matrix other than the one
# on this machine (CI builds with GCC 14 and Clang 18 on Linux); the macOS job, which CI runs only
# once the repository is public; the nightly run of the differential test; and the cold cache of
# the clean-clone job. The README stage builds and runs the commands the README marks, in this
# working tree, so a file that was never committed is not noticed by it.
#
# Everything here is CPU-heavy: it builds the engine four times and runs the flaws matrix. It takes
# long enough that it is worth running when the machine has nothing else to do.
#
# Never pipe a stage into another command. With a pipe the exit status is the last command's, and
# a failing stage followed by `tail` reads as a success.
set -euo pipefail
cd "$(dirname "$0")/.."

# Keep these in step with the lint job in .github/workflows/ci.yml, which uses clang-format-18 and
# clang-tidy-18. The major version is what is checked: a different one formats differently.
CLANG_MAJOR=18

# The fuzz targets need libFuzzer, which Apple's Clang does not ship, so they are built with the
# Clang from Homebrew's llvm. Set these for a Clang installed somewhere else. The fuzz job of CI
# runs each target for 60 seconds; here it is 10.
FUZZ_CC="${FUZZ_CC:-/opt/homebrew/opt/llvm/bin/clang}"
FUZZ_CXX="${FUZZ_CXX:-/opt/homebrew/opt/llvm/bin/clang++}"
FUZZ_SECONDS=10

# This repository must be its own working tree. If it is a subdirectory of a larger one, every path
# and every push is pointing somewhere unintended, so fail before anything runs.
if [ "$(git rev-parse --show-toplevel)" != "$(pwd -P)" ]; then
  echo "preflight: $(pwd -P) is not the repository root ($(git rev-parse --show-toplevel))" >&2
  exit 1
fi

stage() {
  printf '\n==> %s\n' "$*"
}

# require_tool <tool> [<major version>]: the tool must be on PATH and, when a version is given,
# report that major version.
require_tool() {
  local tool=$1 major=${2:-} actual
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "preflight: $tool is not installed" >&2
    if [ -n "$major" ]; then
      echo "preflight: run: uv tool install $tool==$major.1.8" >&2
    fi
    exit 1
  fi
  [ -n "$major" ] || return 0
  # The output goes through a variable, not a pipe into grep -q: grep exits at the first match
  # and the tool then dies of SIGPIPE, which `pipefail` would report as a failure.
  actual="$("$tool" --version 2>&1)"
  case "$actual" in
    *"version $major."*) ;;
    *)
      echo "preflight: $tool must be version $major, found:" >&2
      echo "$actual" >&2
      echo "preflight: run: uv tool install --force $tool==$major.1.8" >&2
      exit 1
      ;;
  esac
}

require_executable() { # <path> <how to get it>
  if [ ! -x "$1" ]; then
    echo "preflight: $1 is not installed; $2" >&2
    exit 1
  fi
}

# build_and_test <preset>: configure, build and run the tests of one preset.
build_and_test() {
  stage "$1: configure, build, test"
  cmake --preset "$1"
  cmake --build --preset "$1"
  ctest --preset "$1"
}

require_tool uv
require_tool clang-format "$CLANG_MAJOR"
require_tool clang-tidy "$CLANG_MAJOR"
require_executable "$FUZZ_CXX" "run: brew install llvm, or set FUZZ_CC and FUZZ_CXX"

stage "clang-format"
git ls-files -z -- '*.h' '*.cc' | xargs -0 -r clang-format --dry-run --Werror

stage "layering and hygiene, and the tests of those scripts"
bash scripts/check-layering.sh
bash scripts/check-hygiene.sh
bash tests/scripts/hygiene_test.sh
bash tests/scripts/layering_test.sh

build_and_test dev
stage "no arrow::compute in the objects"
bash scripts/check-objects.sh build/dev

build_and_test asan

# The README stage builds the release preset, which the stages below use.
stage "the README's commands (they build the release preset)"
bash scripts/run-readme-blocks.sh

stage "differential test: the generator's own tests, then 300 seeds"
uv run --locked python -m unittest discover -s tests/differential
uv run --locked tests/differential/run.py --quarry build/release/tools/quarry --seeds 0:300

# The bench-smoke job of CI: the harness runs to the end on the SF0.01 data, its own tests pass and
# the numbers in the README and the docs are the ones the committed results produce.
stage "bench smoke"
QUARRY_BUILD_DIR=build/release uv run --locked python -m unittest discover -s bench
uv run --locked bench/run_bench.py --suite smoke --data tests/data/tpch-sf0.01 --build-dir build/release --no-build
bash scripts/check-readme-numbers.sh

# The fuzz job of CI, at ten seconds a target and not sixty. A crash leaves its input in
# build/fuzz/crash-*, and the target reproduces it when it is run with that file.
stage "fuzz"
CC="$FUZZ_CC" CXX="$FUZZ_CXX" cmake --preset fuzz
cmake --build build/fuzz --target fuzz_parser fuzz_query
rm -rf build/fuzz/corpus-parser build/fuzz/corpus-query
mkdir -p build/fuzz/corpus-parser build/fuzz/corpus-query
cp tests/tpch/queries/*.sql build/fuzz/corpus-parser/
build/fuzz/fuzz/fuzz_parser build/fuzz/corpus-parser -max_total_time="$FUZZ_SECONDS" -max_len=65536 \
  -artifact_prefix=build/fuzz/crash-
build/fuzz/fuzz/fuzz_query build/fuzz/corpus-query -max_total_time="$FUZZ_SECONDS" -max_len=512 \
  -artifact_prefix=build/fuzz/crash-

stage "clang-tidy"
# The standalone clang-tidy does not ask Xcode where the SDK is, so it cannot find <string>.
if [ "$(uname -s)" = Darwin ] && [ -z "${SDKROOT:-}" ]; then
  SDKROOT="$(xcrun --show-sdk-path)"
  export SDKROOT
fi
cmake -S . -B build/lint -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DQUARRY_BUILD_TESTS=OFF
git ls-files -z -- 'src/*.cc' 'tools/*.cc' |
  xargs -0 -r clang-tidy -p build/lint --quiet -warnings-as-errors='*'

# The slowest stage, so the last: one Release build per injected flaw, each of which must make the
# tests fail. The flaws job of CI runs the same script, one flaw to a job.
stage "injected flaws"
flaws="$(bash scripts/run-flaws.sh --list | sed 's/:.*//')"
# shellcheck disable=SC2086  # the names are words
bash scripts/run-flaws.sh $flaws

echo
echo "preflight: ok"
