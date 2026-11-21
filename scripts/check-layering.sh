#!/usr/bin/env bash
# scripts/check-layering.sh [root]
#
# Enforces, on the #include lines and the code under include/ and src/:
#   1. the include direction between the directories of src/ (the table in layer_allows below,
#      which is the link graph of src/CMakeLists.txt with its transitive closure)
#   2. no <arrow/compute/...> include and no `arrow::compute` symbol: quarry computes with its own
#      kernels, so that the engine is the thing that is measured (docs/design/0003-arrow-boundary.md)
#   3. no Arrow or Parquet header in src/parser: the parser has no Arrow
#   4. library code does not write to stdout or stderr, and nothing throws
# The ban of rule 2 on compiled code is scripts/check-objects.sh, which runs after the build.
# `root` defaults to the repository root. A violation is printed as `path:line: message`.
#
# The script fails closed. Exit status: 0 clean, 1 violations, 2 nothing to check. A directory of
# src/ that has no entry in the table is itself a violation: a layer without a rule would be
# silently skipped otherwise.
set -uo pipefail
export LC_ALL=C

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$root" 2>/dev/null || {
  echo "check-layering: cannot enter $root" >&2
  exit 2
}
dirs=()
[ -d include ] && dirs+=(include)
[ -d src ] && dirs+=(src)
if [ "${#dirs[@]}" -eq 0 ]; then
  echo "check-layering: neither include/ nor src/ exists under $root" >&2
  exit 2
fi

violations=0
report() {
  printf '%s:%s: %s\n' "$1" "$2" "$3"
  violations=$((violations + 1))
}

# layer_allows <layer>: the project directories a layer may include from, itself included.
layer_allows() {
  case "$1" in
    types) echo "types" ;;
    vector) echo "types vector" ;;
    expr) echo "types vector expr" ;;
    parser) echo "parser" ;;
    binder) echo "parser types vector expr io plan binder" ;;
    plan) echo "types vector expr plan" ;;
    planner) echo "binder parser types vector expr plan planner" ;;
    io) echo "types vector io" ;;
    exec) echo "plan types vector expr io exec" ;;
    engine) echo "binder parser types vector expr plan planner io exec engine" ;;
    common) echo "common" ;;
    *) return 1 ;;
  esac
}

# grep_lines <grep options> <regex> <file>: leaves the numbered matching lines in $matches. Status 1
# is "no match"; anything above 1 (an unreadable file, a bad pattern) is a failure of the check
# itself and ends it, because a scan that could not read a file must not say "ok".
matches=""
grep_lines() {
  local status errfile
  errfile="$(mktemp)"
  matches="$(grep "-n$1" -e "$2" -- "$3" 2>"$errfile")"
  status=$?
  if [ "$status" -gt 1 ]; then
    echo "check-layering: grep failed with status $status on $3" >&2
    cat "$errfile" >&2
    rm -f "$errfile"
    exit 1
  fi
  rm -f "$errfile"
}

include_re='^[[:space:]]*#[[:space:]]*include[[:space:]]*(["<])([^">]+)[">]'

layer_of() { # <file>
  case "$1" in
    src/*/*)
      local rest="${1#src/}"
      echo "${rest%%/*}"
      ;;
    include/*) echo public ;;
    *) echo unknown ;;
  esac
}

# A line that is only a comment cannot run or include anything.
is_comment_line() {
  case "$1" in
    //*) return 0 ;;
    '/*'*) case "$1" in *'*/'*[!/]*) return 1 ;; *) return 0 ;; esac ;;
    '*' | '* '* | '*/') return 0 ;;
  esac
  return 1
}

trim() {
  local t="${1#"${1%%[![:space:]]*}"}"
  printf '%s' "${t%"${t##*[![:space:]]}"}"
}

# code_of <line>: the line without string literals, one-line block comments and // comments.
code_of() {
  sed -E -e 's/"([^"\\]|\\.)*"//g' -e 's,/\*([^*]|\*[^/])*\*/,,g' -e 's,//.*$,,' <<<"$1"
}

check_includes() {
  local file=$1 layer=$2 lineno text kind spec top allowed
  if [ "$layer" != public ]; then
    if ! allowed="$(layer_allows "$layer")"; then
      report "$file" 1 "unknown layer '$layer': add it to scripts/check-layering.sh"
      return
    fi
  fi
  grep_lines E '^[[:space:]]*#[[:space:]]*include' "$file"
  while IFS=: read -r lineno text; do
    [[ $text =~ $include_re ]] || continue
    kind="${BASH_REMATCH[1]}"
    spec="${BASH_REMATCH[2]}"
    case "$spec" in
      arrow/compute | arrow/compute/*)
        report "$file" "$lineno" "arrow/compute is banned: quarry has its own kernels"
        continue
        ;;
    esac
    if [ "$layer" = parser ]; then
      case "$spec" in
        arrow/* | parquet/*) report "$file" "$lineno" "src/parser has no Arrow: found <$spec>" ;;
      esac
    fi
    [ "$kind" = '<' ] && continue
    top="${spec%%/*}"
    case "$spec" in
      *..*) report "$file" "$lineno" "\"$spec\" climbs out of its directory"; continue ;;
      */*) ;;
      *) continue ;; # a header of the same directory
    esac
    if [ "$layer" = public ]; then
      [ "$top" = quarry ] || report "$file" "$lineno" "include/ may include only quarry/*: found \"$spec\""
      continue
    fi
    case "$top" in
      quarry)
        [ "$layer" = engine ] || report "$file" "$lineno" "only src/engine includes the public header: found \"$spec\""
        ;;
      common) ;;
      *)
        case " $allowed " in
          *" $top "*) ;;
          *) report "$file" "$lineno" "src/$layer must not include src/$top: found \"$spec\"" ;;
        esac
        ;;
    esac
  done <<<"$matches"
}

# scan_code <file> <regex> <message>: reports each line whose code (not comment, not string) matches.
scan_code() {
  local file=$1 re=$2 message=$3 lineno text trimmed code
  grep_lines E "$re" "$file"
  while IFS=: read -r lineno text; do
    [ -n "$lineno" ] || continue
    trimmed="$(trim "$text")"
    is_comment_line "$trimmed" && continue
    code="$(code_of "$text")"
    [[ $code =~ $re ]] || continue
    report "$file" "$lineno" "$message"
  done <<<"$matches"
}

output_re='std::(cout|cerr)([^[:alnum:]_]|$)|(^|[^[:alnum:]_])(printf|fprintf|puts)[[:space:]]*\('
throw_re='(^|[^[:alnum:]_])throw([^[:alnum:]_]|$)'
compute_re='arrow::compute([^[:alnum:]_]|$)'

files="$(find "${dirs[@]}" -type f \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' \) | sort)" || {
  echo "check-layering: find failed under ${dirs[*]}" >&2
  exit 1
}

checked=0
while IFS= read -r file; do
  [ -n "$file" ] || continue
  checked=$((checked + 1))
  check_includes "$file" "$(layer_of "$file")"
  scan_code "$file" "$compute_re" "arrow::compute is banned: quarry has its own kernels"
  scan_code "$file" "$output_re" "library code must not write to stdout or stderr"
  scan_code "$file" "$throw_re" "nothing under include/ or src/ may throw: errors are returned as a Status"
done <<<"$files"

if [ "$checked" -eq 0 ]; then
  echo "check-layering: no C++ files under ${dirs[*]} in $root" >&2
  exit 2
fi
if [ "$violations" -ne 0 ]; then
  echo "check-layering: $violations violation(s) in $checked files" >&2
  exit 1
fi
echo "check-layering: ok ($checked files)"
