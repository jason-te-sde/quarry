#!/usr/bin/env bash
# tests/scripts/readme_blocks_test.sh
#
# Tests scripts/run-readme-blocks.sh on throwaway README files: marked blocks run in order, unmarked
# blocks do not run, the first failure ends the run with status 1, and every way a file can fail to
# say what to run (no block, a marker with nothing after it, an unclosed block) is status 2. The
# blocks write to a log, so the order and the absence of the unmarked ones can be read off it.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
script="$(cd "$here/../.." && pwd)/scripts/run-readme-blocks.sh"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0
fail() {
  echo "FAIL: $1"
  failures=$((failures + 1))
}

# check <case> <expected status> <expected log> <readme text>
check() {
  local name=$1 want=$2 log_want=$3 text=$4 status log
  printf '%s\n' "$text" >"$work/readme.md"
  : >"$work/log"
  RUN_LOG="$work/log" bash "$script" "$work/readme.md" >/dev/null 2>&1
  status=$?
  log="$(tr '\n' ' ' <"$work/log")"
  [ "$status" -eq "$want" ] || fail "$name: exit status $status, wanted $want"
  [ "$log" = "$log_want" ] || fail "$name: ran [$log], wanted [$log_want]"
}

fence='```'

check "two blocks run in order" 0 "one two " \
  "# t
<!-- run -->
${fence}sh
echo one >>\"\$RUN_LOG\"
${fence}

text

<!-- run -->

${fence}bash
echo two >>\"\$RUN_LOG\"
${fence}"

check "an unmarked block does not run" 0 "marked " \
  "${fence}sh
echo unmarked >>\"\$RUN_LOG\"
${fence}
<!-- run -->
${fence}sh
echo marked >>\"\$RUN_LOG\"
${fence}"

check "the first failure ends the run" 1 "before " \
  "<!-- run -->
${fence}sh
echo before >>\"\$RUN_LOG\"
false
echo after-in-block >>\"\$RUN_LOG\"
${fence}
<!-- run -->
${fence}sh
echo second >>\"\$RUN_LOG\"
${fence}"

check "a failing command in the middle of a block fails it (set -e)" 1 "" \
  "<!-- run -->
${fence}sh
false
echo hidden >>\"\$RUN_LOG\"
${fence}"

check "a failure inside a pipe fails the block (pipefail)" 1 "" \
  "<!-- run -->
${fence}sh
false | cat
${fence}"

check "no marked block is an error" 2 "" \
  "${fence}sh
echo unmarked >>\"\$RUN_LOG\"
${fence}"

check "a marker followed by text is an error" 2 "" \
  "<!-- run -->
text
${fence}sh
echo x >>\"\$RUN_LOG\"
${fence}"

check "a marker at the end of the file is an error" 2 "" \
  "<!-- run -->"

check "an unclosed block is an error" 2 "" \
  "<!-- run -->
${fence}sh
echo x >>\"\$RUN_LOG\""

check "a block of another language is an error" 2 "" \
  "<!-- run -->
${fence}python
print(1)
${fence}"

bash "$script" "$work/does-not-exist.md" >/dev/null 2>&1
[ "$?" -eq 2 ] || fail "a file that cannot be read must be status 2"

# The README of this repository has marked blocks, and they are well formed.
repo_readme="$(cd "$here/../.." && pwd)/README.md"
marked="$(grep -c '^<!-- run -->$' "$repo_readme")"
[ "$marked" -ge 1 ] || fail "README.md has no <!-- run --> block"

if [ "$failures" -eq 0 ]; then
  echo "ok: run-readme-blocks.sh"
  exit 0
fi
exit 1
