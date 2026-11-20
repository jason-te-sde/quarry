#!/usr/bin/env bash
# tests/scripts/layering_test.sh
#
# Tests scripts/check-layering.sh against throwaway trees: a clean tree must pass, each rule must be
# broken by one offending file and make the script exit 1, and a file grep cannot read, or a tree
# with nothing to check, must not say "ok".
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
script="$(cd "$here/../.." && pwd)/scripts/check-layering.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0
expect_status() { # <case> <expected status> <root>
  local status
  bash "$script" "$3" >/dev/null 2>&1
  status=$?
  if [ "$status" -ne "$2" ]; then
    echo "FAIL: $1: expected exit $2, got $status"
    failures=$((failures + 1))
  else
    echo "ok:   $1"
  fi
}

# put_file <case> <path under the tree> <line>: a tree of the case with one file.
put_file() {
  mkdir -p "$work/$1/$(dirname "$2")"
  printf '%s\n' "$3" >"$work/$1/$2"
}

put_file clean src/exec/ok.cc '#include "plan/logical.h"'
put_file clean src/io/ok.cc '#include <arrow/type.h>'
expect_status "a clean tree passes" 0 "$work/clean"

put_file compute-include src/exec/a.cc '#include <arrow/compute/api.h>'
expect_status "an arrow/compute include fails" 1 "$work/compute-include"

put_file compute-symbol src/exec/a.cc 'void F() { auto s = arrow::compute::Sum; }'
expect_status "an arrow::compute symbol fails" 1 "$work/compute-symbol"

put_file parser-arrow src/parser/a.cc '#include <arrow/type.h>'
expect_status "Arrow in src/parser fails" 1 "$work/parser-arrow"

put_file direction src/planner/a.cc '#include "exec/operator.h"'
expect_status "planner including exec fails" 1 "$work/direction"

put_file direction-up src/types/a.cc '#include "vector/batch.h"'
expect_status "types including vector fails" 1 "$work/direction-up"

put_file unknown-layer src/mystery/a.cc '#include <vector>'
expect_status "a directory without a rule fails" 1 "$work/unknown-layer"

put_file output src/exec/a.cc 'void F() { std::cout << 1; }'
expect_status "writing to stdout fails" 1 "$work/output"

put_file throw src/exec/a.cc 'void F() { throw 1; }'
expect_status "throw fails" 1 "$work/throw"

put_file public include/quarry/a.h '#include "exec/operator.h"'
expect_status "the public header including src/ fails" 1 "$work/public"

# A comment that mentions a banned thing is not a violation.
put_file comment src/exec/a.cc '// arrow::compute is banned, and so is throw'
expect_status "a comment naming a ban passes" 0 "$work/comment"

mkdir -p "$work/nothing/src"
expect_status "a tree without C++ files is not checked" 2 "$work/nothing"
expect_status "a root without src/ is not checked" 2 "$work"/does-not-exist

# An unreadable file: grep exits 2 on it, and the script must fail rather than pass over it. Root
# reads everything, so there the case cannot be built and is skipped.
put_file unreadable src/exec/a.cc '#include "plan/logical.h"'
chmod 000 "$work/unreadable/src/exec/a.cc"
if [ -r "$work/unreadable/src/exec/a.cc" ]; then
  echo "skip: an unreadable file fails (the user can read it anyway)"
else
  expect_status "an unreadable file fails" 1 "$work/unreadable"
fi

if [ "$failures" -ne 0 ]; then
  echo "layering_test: $failures case(s) failed" >&2
  exit 1
fi
echo "layering_test: all cases passed"
