#!/usr/bin/env bash
# tests/scripts/flaws_test.sh
#
# Tests the argument handling of scripts/run-flaws.sh without building anything, and that its
# matrix covers exactly the flaws that src/common/flaws.h defines, each with a catching test.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
script="$root/scripts/run-flaws.sh"

failures=0
fail() {
  echo "FAIL: $1"
  failures=$((failures + 1))
}

bash "$script" >/dev/null 2>&1
[ "$?" -eq 2 ] || fail "no argument must be a usage error (status 2)"

bash "$script" no_such_flaw >/dev/null 2>&1
[ "$?" -eq 2 ] || fail "an unknown flaw must be a usage error (status 2)"

listed="$(bash "$script" --list | sed 's/:.*//' | sort)"
defined="$(grep -o 'QUARRY_FLAW_[A-Z][A-Z_]*' "$root/src/common/flaws.h" | sort -u | sed 's/^QUARRY_FLAW_//' |
  tr 'A-Z' 'a-z' | sort)"
[ -n "$defined" ] || fail "no flaw found in src/common/flaws.h"
if [ "$listed" != "$defined" ]; then
  fail "the matrix and src/common/flaws.h disagree: matrix [$(echo $listed)] header [$(echo $defined)]"
fi

while IFS= read -r line; do
  catchers="${line#*:}"
  [ -n "$(echo "$catchers" | tr -d ' ')" ] || fail "no catching test for ${line%%:*}"
done < <(bash "$script" --list)

if [ "$failures" -eq 0 ]; then
  echo "ok: run-flaws.sh"
  exit 0
fi
exit 1
