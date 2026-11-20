#!/usr/bin/env bash
# scripts/check-objects.sh [build_dir]
#
# The compiled half of the Arrow compute ban (docs/design/0003-arrow-boundary.md): no object that
# quarry itself compiled may define or reference an `arrow::compute` symbol. It runs `nm -C` over
# every libquarry_*.a archive and over the objects of the `quarry` tool under `build_dir` (default
# build/dev) and fails on a match. Run it after the build. The source half is check-layering.sh.
#
# The script fails closed. Exit status: 0 clean, 1 a violation or a failure of nm, 2 nothing to
# check (no archive and no tool object), which would otherwise pass vacuously.
set -uo pipefail
export LC_ALL=C

build="${1:-build/dev}"
[ -d "$build" ] || {
  echo "check-objects: $build is not a directory" >&2
  exit 2
}

objects="$(find "$build" -type f \( -name 'libquarry_*.a' -o -path '*/CMakeFiles/quarry.dir/*.o' \) | sort)" || {
  echo "check-objects: find failed under $build" >&2
  exit 1
}
if [ -z "$objects" ]; then
  echo "check-objects: no libquarry_*.a archive and no quarry tool object under $build" >&2
  exit 2
fi

errfile="$(mktemp)"
trap 'rm -f "$errfile"' EXIT

violations=0
count=0
while IFS= read -r object; do
  count=$((count + 1))
  symbols="$(nm -C "$object" 2>"$errfile")"
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "check-objects: nm failed with status $status on $object" >&2
    cat "$errfile" >&2
    exit 1
  fi
  hits="$(grep -F 'arrow::compute' <<<"$symbols")"
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" | sed "s|^|$object: |" | head -n 5
    violations=$((violations + 1))
  fi
done <<<"$objects"

if [ "$violations" -ne 0 ]; then
  echo "check-objects: $violations object(s) reference arrow::compute" >&2
  exit 1
fi
echo "check-objects: ok ($count objects)"
