#!/usr/bin/env bash
# scripts/run-readme-blocks.sh [file]
#
# Runs every command block of the README (or of `file`) that is announced by a line holding only
# the HTML comment `<!-- run -->`, in the order they appear, each in a shell of its own with
# `set -euo pipefail`, from the root of the repository. The first block that fails ends the run.
# CI's clean-clone job calls it on a fresh checkout after it has built Arrow, so that what the README
# tells a newcomer to type is what is tested.
#
# A marked block is the fenced block (```sh or ```bash) that follows the marker, with blank lines
# allowed in between. Unmarked blocks are only shown, not run.
#
# The script fails closed. Exit status: 0 every marked block succeeded, 1 a block failed, 2 the
# file could not be read, a marker is not followed by a block, a block is never closed, or there is
# no marked block at all (a README with nothing to run must not say "ok").
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
file="${1:-$root/README.md}"
case "$file" in
  /*) ;;
  *) file="$PWD/$file" ;;
esac
if [ ! -r "$file" ]; then
  echo "run-readme-blocks: cannot read $file" >&2
  exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

count="$(awk -v dir="$work" '
  function bad(message) {
    print "run-readme-blocks: " message > "/dev/stderr"
    failed = 1
    exit 2
  }
  {
    line = $0
    sub(/\r$/, "", line)
  }
  state == 0 {
    if (line == "<!-- run -->") {
      state = 1
      marker_line = NR
    }
    next
  }
  state == 1 {
    if (line ~ /^[ \t]*$/) {
      next
    }
    if (line == "```sh" || line == "```bash") {
      n++
      out = sprintf("%s/block-%03d.sh", dir, n)
      state = 2
      open_line = NR
      next
    }
    bad("line " marker_line ": <!-- run --> must be followed by a ```sh or ```bash block")
  }
  state == 2 {
    if (line == "```") {
      close(out)
      state = 0
      next
    }
    print line >> out
  }
  END {
    if (failed) {
      exit 2
    }
    if (state == 1) {
      bad("line " marker_line ": <!-- run --> is the last thing in the file")
    }
    if (state == 2) {
      bad("line " open_line ": the block is never closed")
    }
    print n + 0
  }' "$file")"
status=$?
if [ "$status" -ne 0 ]; then
  exit 2
fi
if [ "$count" -eq 0 ]; then
  echo "run-readme-blocks: $file has no <!-- run --> block, so there is nothing to run" >&2
  exit 2
fi

cd "$root" || exit 2
i=0
for block in "$work"/block-*.sh; do
  i=$((i + 1))
  echo "run-readme-blocks: block $i of $count"
  sed 's/^/  | /' "$block"
  if ! bash -euo pipefail "$block"; then
    echo "run-readme-blocks: block $i failed" >&2
    exit 1
  fi
done
echo "run-readme-blocks: ok ($count blocks)"
