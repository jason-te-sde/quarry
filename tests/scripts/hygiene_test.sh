#!/usr/bin/env bash
# tests/scripts/hygiene_test.sh
#
# Tests scripts/check-hygiene.sh against throwaway repositories: a clean one must pass, and one
# offending file per rule must each make the script exit 1. A rule that silently stopped matching
# would turn the corresponding case green, which is the failure this test exists to catch.
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
repo_root="$(cd "$here/../.." && pwd)"
script="$repo_root/scripts/check-hygiene.sh"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0

# make_repo <name>: a repository with scripts/ copied in and one clean tracked file.
make_repo() {
  local dir="$work/$1"
  mkdir -p "$dir/scripts"
  cp "$repo_root/scripts/check-hygiene.sh" "$repo_root/scripts/attribution-patterns.txt" "$dir/scripts/"
  git -C "$dir" init -q
  git -C "$dir" config user.name tester
  git -C "$dir" config user.email tester@example.invalid
  printf 'int main() { return 0; }\n' >"$dir/clean.cc"
  echo "$dir"
}

commit_all() { # <dir> <message>
  git -C "$1" add -A
  git -C "$1" commit -q -m "$2"
}

# expect_status <case> <dir> <expected status>
expect_status() {
  local name=$1 dir=$2 expected=$3 status
  bash "$script" "$dir" >/dev/null 2>&1
  status=$?
  if [ "$status" -ne "$expected" ]; then
    echo "FAIL: $name: expected exit $expected, got $status"
    failures=$((failures + 1))
  else
    echo "ok:   $name"
  fi
}

dir="$(make_repo clean)"
commit_all "$dir" "add a clean file"
expect_status "clean repository passes" "$dir" 0

# Rule 1: a CJK character (U+4E2D, written as bytes so that this file stays ASCII).
dir="$(make_repo cjk)"
printf '// \xe4\xb8\xad\n' >"$dir/cjk.cc"
commit_all "$dir" "add a file"
expect_status "CJK in a tracked file fails" "$dir" 1

# Rule 2: a credit trailer, in a commit message. The credit text is assembled from pieces so that
# this file does not contain a credit itself.
dir="$(make_repo trailer)"
commit_all "$dir" "add a file

Co-Auth""ored-By: Someone <someone@example.invalid>"
expect_status "credit trailer in a commit message fails" "$dir" 1

# Rule 2 again, in a tracked file.
dir="$(make_repo credit-file)"
printf '// Gener''ated by a tool\n' >"$dir/credit.cc"
commit_all "$dir" "add a file"
expect_status "credit line in a tracked file fails" "$dir" 1

# Rule 3: an absolute home path. The path is assembled from pieces so that this file does not
# contain one itself.
dir="$(make_repo abs-path)"
printf '// see /Us''ers/someone/x\n' >"$dir/path.cc"
commit_all "$dir" "add a file"
expect_status "absolute home path in a tracked file fails" "$dir" 1

dir="$(make_repo abs-path-message)"
commit_all "$dir" "read /ho""me/someone/x"
expect_status "absolute home path in a commit message fails" "$dir" 1

# A directory that is not a git working tree has nothing to check, which is status 2, never "ok".
mkdir -p "$work/not-a-repo"
expect_status "a non-repository is not checked" "$work/not-a-repo" 2

if [ "$failures" -ne 0 ]; then
  echo "hygiene_test: $failures case(s) failed" >&2
  exit 1
fi
echo "hygiene_test: all cases passed"
