#!/usr/bin/env bash
# tests/scripts/cli_test.sh <path-to-quarry>
#
# Runs the command-line tool against the TPC-H test data and checks its output and exit status.
# Run from the repository root.
set -uo pipefail

quarry="${1:?usage: cli_test.sh <path-to-quarry>}"
data=tests/data/tpch-sf0.01
failures=0
stderr_file="$(mktemp)"
trap 'rm -f "$stderr_file"' EXIT

fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}

# expect_status <name> <expected status> <command...>: runs it, leaves stdout in $out.
expect_status() {
  local name=$1 want=$2
  shift 2
  out="$("$@" 2>"$stderr_file")"
  local got=$?
  [ "$got" -eq "$want" ] || fail "$name: exit status $got, expected $want (stderr: $(cat "$stderr_file"))"
}

expect_contains() { # <name> <text> <needle>
  case "$2" in *"$3"*) ;; *) fail "$1: expected to find '$3' in: $2" ;; esac
}

expect_stderr_contains() { # <name> <needle>
  case "$(cat "$stderr_file")" in *"$2"*) ;; *) fail "$1: expected stderr to contain '$2', got: $(cat "$stderr_file")" ;; esac
}

q="SELECT r_regionkey, r_name FROM region WHERE r_regionkey < 2 ORDER BY r_regionkey"

expect_status "table format" 0 "$quarry" query --data "$data" -c "$q"
expected_table=$'r_regionkey | r_name\n------------+--------\n          0 | AFRICA\n          1 | AMERICA\n(2 rows)'
[ "$out" = "$expected_table" ] || fail "table format: got: $out"

expect_status "json format" 0 "$quarry" query --data "$data" -c "$q" --format json
[ "$out" = '{"columns":[{"name":"r_regionkey","type":"INTEGER"},{"name":"r_name","type":"VARCHAR"}],"rows":[[0,"AFRICA"],[1,"AMERICA"]]}' ] ||
  fail "json format: got: $out"

expect_status "csv format" 0 "$quarry" query --data "$data" -c "$q" --format csv
[ "$out" = $'r_regionkey,r_name\n0,AFRICA\n1,AMERICA' ] || fail "csv format: got: $out"

printf '%s\n' "$q" >"$(dirname "$stderr_file")/quarry-cli-query.sql"
expect_status "query from a file" 0 "$quarry" query --data "$data" -f "$(dirname "$stderr_file")/quarry-cli-query.sql" --format csv
[ "$out" = $'r_regionkey,r_name\n0,AFRICA\n1,AMERICA' ] || fail "query from a file: got: $out"
rm -f "$(dirname "$stderr_file")/quarry-cli-query.sql"

expect_status "batch size, memory limit, disabled rule" 0 "$quarry" query --data "$data" -c "$q" \
  --format csv --batch-size 1 --memory-limit 64MiB --disable-rule push_filters --disable-rule order_joins
[ "$out" = $'r_regionkey,r_name\n0,AFRICA\n1,AMERICA' ] || fail "options: got: $out"

expect_status "explain" 0 "$quarry" query --data "$data" -c "$q" --explain
expect_contains "explain" "$out" "Scan #"
expect_contains "explain" "$out" "est="

expect_status "explain analyze" 0 "$quarry" query --data "$data" -c "$q" --explain-analyze
expect_contains "explain analyze" "$out" " rows=2 "
expect_contains "explain analyze" "$out" " time="

expect_status "a syntax error" 1 "$quarry" query --data "$data" -c "SELEC 1"
expect_stderr_contains "a syntax error" "syntax error at 1:1"
expect_status "a binder error" 1 "$quarry" query --data "$data" -c "SELECT nope FROM region"
expect_stderr_contains "a binder error" "binder error at 1:8"
expect_status "an unknown rule" 1 "$quarry" query --data "$data" -c "$q" --disable-rule nope
expect_stderr_contains "an unknown rule" "nope"
expect_status "a missing data directory" 1 "$quarry" query --data tests/data/no-such-dir -c "$q"

expect_status "no --data" 2 "$quarry" query -c "$q"
expect_status "no query" 2 "$quarry" query --data "$data"
expect_status "both -c and -f" 2 "$quarry" query --data "$data" -c "$q" -f /dev/null
expect_status "a bad format" 2 "$quarry" query --data "$data" -c "$q" --format xml
expect_status "a bad memory limit" 2 "$quarry" query --data "$data" -c "$q" --memory-limit lots
expect_status "a bad batch size" 2 "$quarry" query --data "$data" -c "$q" --batch-size 0
expect_status "a bad timeout" 2 "$quarry" query --data "$data" -c "$q" --timeout soon
expect_status "an unknown option" 2 "$quarry" query --data "$data" -c "$q" --frobnicate
expect_status "a missing value" 2 "$quarry" query --data "$data" -c
expect_status "no command" 2 "$quarry"
expect_status "help" 0 "$quarry" --help
expect_contains "help" "$out" "--memory-limit"
expect_status "a missing query file" 1 "$quarry" query --data "$data" -f tests/no-such-file.sql

# --repeat: one warm-up and N measured executions in one process, timed from the SQL text to the
# materialised table, written as JSON. Nothing is printed.
timing="$(dirname "$stderr_file")/quarry-cli-timing.json"
expect_status "repeat" 0 "$quarry" query --data "$data" -c "$q" --repeat 3 --timing-json "$timing"
[ -z "$out" ] || fail "repeat: expected no output, got: $out"
timing_text="$(cat "$timing")"
expect_contains "repeat" "$timing_text" '"warmup":{"ns":'
expect_contains "repeat" "$timing_text" '"peak_rss_bytes":'
expect_contains "repeat" "$timing_text" '"pool_peak_bytes":'
expect_contains "repeat" "$timing_text" '"scan_pool_peak_bytes":'
expect_contains "repeat" "$timing_text" '"spilled_bytes":0'
expect_contains "repeat" "$timing_text" '"rows":2'
[ "$(printf '%s' "$timing_text" | grep -o '"load_1m"' | wc -l | tr -d ' ')" -eq 4 ] ||
  fail "repeat: expected a load average for the warm-up and each of 3 runs: $timing_text"
expect_status "repeat, a failing query" 1 "$quarry" query --data "$data" -c "SELECT nope FROM region" \
  --repeat 2 --timing-json "$timing"
expect_contains "repeat, a failing query" "$(cat "$timing")" '"error":"binder error at 1:8'
expect_status "repeat without --timing-json" 2 "$quarry" query --data "$data" -c "$q" --repeat 2
expect_status "--timing-json without --repeat" 2 "$quarry" query --data "$data" -c "$q" --timing-json "$timing"
expect_status "repeat with explain" 2 "$quarry" query --data "$data" -c "$q" --repeat 2 --timing-json "$timing" --explain
expect_status "a bad repeat" 2 "$quarry" query --data "$data" -c "$q" --repeat 0 --timing-json "$timing"
rm -f "$timing"

# The schema command keeps working.
expect_status "schema" 0 "$quarry" schema "$data/region.parquet"
expect_contains "schema" "$out" "r_name"

# A query that joins lineitem with itself on a three-valued key runs for a long time.
long="SELECT COUNT(*) FROM lineitem a JOIN lineitem b ON a.l_returnflag = b.l_returnflag"
expect_status "a timeout" 1 "$quarry" query --data "$data" -c "$long" --timeout 50ms
expect_stderr_contains "a timeout" "timeout of 50 ms"

"$quarry" query --data "$data" -c "$long" >/dev/null 2>"$stderr_file" &
pid=$!
sleep 1
kill -INT "$pid" 2>/dev/null
wait "$pid"
status=$?
[ "$status" -eq 130 ] || fail "SIGINT: exit status $status, expected 130"

if [ "$failures" -ne 0 ]; then
  echo "cli_test: $failures failure(s)" >&2
  exit 1
fi
echo "cli_test: ok"
