#!/usr/bin/env bash
# tests/scripts/objects_test.sh
#
# Tests scripts/check-objects.sh on scratch objects: one that references an arrow::compute symbol
# (as an archive and as a loose tool object) must fail, one that does not must pass, and a build
# directory with nothing to check must not say "ok".
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
script="$(cd "$here/../.." && pwd)/scripts/check-objects.sh"
cxx="${CXX:-c++}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

failures=0
expect_status() { # <case> <expected status> <build dir>
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

# The offending object only declares the symbol and calls it, so that it needs no Arrow install: the
# undefined reference to arrow::compute::Sum is what the check must see.
cat >"$work/bad.cc" <<'SRC'
namespace arrow::compute {
void Sum();
}
void UseCompute() { arrow::compute::Sum(); }
SRC
cat >"$work/good.cc" <<'SRC'
namespace arrow {
void Other();
}
void UseOther() { arrow::Other(); }
SRC
"$cxx" -std=c++20 -c "$work/bad.cc" -o "$work/bad.o" || { echo "cannot compile the scratch object" >&2; exit 1; }
"$cxx" -std=c++20 -c "$work/good.cc" -o "$work/good.o" || { echo "cannot compile the scratch object" >&2; exit 1; }

mkdir -p "$work/clean/src" "$work/archive/src" "$work/tool/tools/CMakeFiles/quarry.dir" "$work/empty/src"
ar rcs "$work/clean/src/libquarry_x.a" "$work/good.o"
ar rcs "$work/archive/src/libquarry_x.a" "$work/bad.o"
cp "$work/bad.o" "$work/tool/tools/CMakeFiles/quarry.dir/main.cc.o"
# An archive of somebody else's (not libquarry_*) must not count as quarry's.
ar rcs "$work/empty/src/libother.a" "$work/bad.o"

expect_status "a clean archive passes" 0 "$work/clean"
expect_status "an archive referencing arrow::compute fails" 1 "$work/archive"
expect_status "a tool object referencing arrow::compute fails" 1 "$work/tool"
expect_status "a build directory without quarry objects is not checked" 2 "$work/empty"
expect_status "a missing build directory is not checked" 2 "$work/missing"

if [ "$failures" -ne 0 ]; then
  echo "objects_test: $failures case(s) failed" >&2
  exit 1
fi
echo "objects_test: all cases passed"
