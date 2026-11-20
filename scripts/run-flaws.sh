#!/usr/bin/env bash
# scripts/run-flaws.sh <flaw>...
# scripts/run-flaws.sh --list
#
# The flaws matrix (spec section 5.5). For each named flaw the script builds quarry with
# -DQUARRY_FLAW=<flaw>, which compiles that one deliberate defect in (src/common/flaws.h), runs the
# tests that are meant to catch it, and succeeds only if every one of them FAILS. A flaw that a
# catching test lets through means the suite has a hole, so the script exits 1 and says which test
# was blind.
#
#   flaw                         catching tests
#   selection_off_by_one         EvalProperty_test
#   null_as_false                differential run, 300 seeds
#   decimal_scale_slip           Tpch_test
#   join_order_drops_predicate   JoinOrder_test, Tpch_test
#   drop_hash_collisions         JoinHashTable_test, differential run
#   prune_wrong_bound            Scan_test (PrunesRowGroupsOnBoundaries), differential run
#   spill_drops_last_batch       TpchBudget_test
#   spill_retains_rows           TpchBudget_test, HighCardinalityAgg_test
#
# Each flaw builds in build/flaw-<flaw>. QUARRY_FLAW_BUILD_TYPE (default Release) picks the build
# type, QUARRY_FLAW_JOBS (default 4) the build parallelism, QUARRY_FLAW_SEEDS (default 0:300) the
# seed range of the differential run. The differential run needs uv.
#
# Exit status: 0 every named flaw was caught, 1 a flaw was missed, 2 a usage error or a build that
# failed (a build that does not compile proves nothing, so it is not "caught").
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2

flaws="selection_off_by_one null_as_false decimal_scale_slip join_order_drops_predicate drop_hash_collisions prune_wrong_bound spill_drops_last_batch spill_retains_rows"
build_type="${QUARRY_FLAW_BUILD_TYPE:-Release}"
jobs="${QUARRY_FLAW_JOBS:-4}"
seeds="${QUARRY_FLAW_SEEDS:-0:300}"

# One catching test per line: test:<binary>, gtest:<binary>:<filter> or differential.
catchers() {
  case "$1" in
    selection_off_by_one) echo "test:EvalProperty_test" ;;
    null_as_false) echo "differential" ;;
    decimal_scale_slip) echo "test:Tpch_test" ;;
    join_order_drops_predicate) printf '%s\n' "test:JoinOrder_test" "test:Tpch_test" ;;
    drop_hash_collisions) printf '%s\n' "test:JoinHashTable_test" "differential" ;;
    prune_wrong_bound) printf '%s\n' "gtest:Scan_test:ScanTest.PrunesRowGroupsOnBoundaries" "differential" ;;
    spill_drops_last_batch) echo "test:TpchBudget_test" ;;
    spill_retains_rows) printf '%s\n' "test:TpchBudget_test" "test:HighCardinalityAgg_test" ;;
    *) return 1 ;;
  esac
}

usage() {
  echo "usage: scripts/run-flaws.sh <flaw>... | --list" >&2
  echo "flaws: $flaws" >&2
}

if [ "$#" -eq 0 ]; then
  usage
  exit 2
fi
if [ "$1" = "--list" ]; then
  for flaw in $flaws; do
    echo "$flaw: $(catchers "$flaw" | tr '\n' ' ')"
  done
  exit 0
fi
for flaw in "$@"; do
  catchers "$flaw" >/dev/null || {
    echo "run-flaws: unknown flaw '$flaw'" >&2
    usage
    exit 2
  }
done

log="$(mktemp)"
trap 'rm -f "$log"' EXIT

run_flaw() {
  local flaw=$1 dir="build/flaw-$1" targets="" entry binary
  while IFS= read -r entry; do
    case "$entry" in
      test:*) targets="$targets ${entry#test:}" ;;
      gtest:*) binary="${entry#gtest:}"; targets="$targets ${binary%%:*}" ;;
      differential) targets="$targets quarry" ;;
    esac
  done < <(catchers "$flaw")

  echo "== $flaw: building $dir ($build_type)"
  if ! cmake -S . -B "$dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type" -DQUARRY_FLAW="$flaw" \
      -DQUARRY_BUILD_TESTS=ON >"$log" 2>&1 ||
    ! cmake --build "$dir" -j "$jobs" --target $targets >>"$log" 2>&1; then
    tail -n 40 "$log" >&2
    echo "flaw $flaw: BUILD FAILED" >&2
    return 2
  fi

  local missed=0 status
  while IFS= read -r entry; do
    case "$entry" in
      test:*)
        binary="${entry#test:}"
        "$dir/tests/$binary" >"$log" 2>&1
        status=$?
        ;;
      gtest:*)
        binary="${entry#gtest:}"
        "$dir/tests/${binary%%:*}" --gtest_filter="${binary#*:}" >"$log" 2>&1
        status=$?
        ;;
      differential)
        binary="differential ($seeds)"
        uv run tests/differential/run.py --quarry "$dir/tools/quarry" --seeds "$seeds" \
          --stop-on-failure >"$log" 2>&1
        status=$?
        # run.py exits 1 on a mismatch; any other failure is a broken run, not a catch.
        if [ "$status" -ne 0 ] && [ "$status" -ne 1 ]; then
          tail -n 20 "$log" >&2
          echo "flaw $flaw: the differential run itself failed (status $status)" >&2
          return 2
        fi
        ;;
    esac
    if [ "$status" -eq 0 ]; then
      echo "   ${binary}: passed, the flaw slipped through"
      tail -n 5 "$log" | sed 's/^/      /'
      missed=1
    else
      echo "   ${binary}: failed (status $status), as it should"
    fi
  done < <(catchers "$flaw")

  if [ "$missed" -eq 0 ]; then
    echo "flaw $flaw: caught"
    return 0
  fi
  echo "flaw $flaw: NOT CAUGHT"
  return 1
}

overall=0
for flaw in "$@"; do
  run_flaw "$flaw"
  status=$?
  if [ "$status" -gt "$overall" ]; then overall=$status; fi
done
exit "$overall"
