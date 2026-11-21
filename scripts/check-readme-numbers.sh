#!/usr/bin/env bash
# scripts/check-readme-numbers.sh
#
# Re-renders every benchmark table of the README and the docs from the newest committed result of
# its suite (bench/results/) and fails when a table in the working tree differs from the render.
# The same holds for a number in prose marked `<!-- n:NAME -->..<!-- /n -->`. A number in a table or
# in marked prose that no committed result produced is therefore a failure.
#
# Exit status: 0 in sync, 1 a table differs, 2 the check could not run.
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2
command -v uv >/dev/null 2>&1 || {
  echo "check-readme-numbers: uv is required" >&2
  exit 2
}
uv run --locked bench/render_tables.py --check
status=$?
[ "$status" -eq 0 ] && echo "check-readme-numbers: ok"
exit "$status"
