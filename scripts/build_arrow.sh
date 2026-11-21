#!/usr/bin/env bash
# scripts/build_arrow.sh [prefix]
#
# Builds Apache Arrow and Parquet 26.0.1 from the official source release and installs them into
# `prefix`, which defaults to $HOME/.local/opt/arrow-26.0.1-<compiler-id> (for example
# arrow-26.0.1-appleclang-21 or arrow-26.0.1-gcc-14). CMake finds that default by itself; any other
# prefix is passed to quarry's configure step as -DQUARRY_ARROW_PREFIX=<prefix>.
#
# The tarball is verified against the .sha512 file published in the same directory of the same
# mirror. A missing checksum or a mismatch is a failure: nothing is built from an unverified source.
# The build and source directories are deleted afterwards; only the install prefix remains, with a
# record of the build in <prefix>/quarry-arrow-build.txt.
#
# Environment: CXX/CC select the compiler (default c++/cc), JOBS the parallelism (default 4),
# QUARRY_KEEP_ARROW_BUILD=1 keeps the build directory for debugging.
set -euo pipefail

version=26.0.1
tarball="apache-arrow-${version}.tar.gz"
mirrors=(
  "https://downloads.apache.org/arrow/arrow-${version}"
  "https://archive.apache.org/dist/arrow/arrow-${version}"
)

cxx="${CXX:-c++}"
cc="${CC:-cc}"
jobs="${JOBS:-4}"

compiler_id() {
  local banner major
  banner="$("$cxx" --version 2>&1 | head -n 1)"
  case "$banner" in
    *"Apple clang"*)
      major="$(sed -E 's/.*version ([0-9]+).*/\1/' <<<"$banner")"
      echo "appleclang-$major"
      ;;
    *clang*)
      major="$(sed -E 's/.*version ([0-9]+).*/\1/' <<<"$banner")"
      echo "clang-$major"
      ;;
    *)
      major="$("$cxx" -dumpversion | cut -d. -f1)"
      echo "gcc-$major"
      ;;
  esac
}

id="$(compiler_id)"
prefix="${1:-$HOME/.local/opt/arrow-${version}-${id}}"

flags=(
  -DCMAKE_BUILD_TYPE=Release
  -DARROW_BUILD_STATIC=ON
  -DARROW_BUILD_SHARED=OFF
  -DARROW_PARQUET=ON
  -DARROW_IPC=ON
  -DARROW_WITH_SNAPPY=ON
  -DARROW_WITH_ZSTD=ON
  -DARROW_COMPUTE=OFF
  -DARROW_CSV=OFF
  -DARROW_JSON=OFF
  -DARROW_DATASET=OFF
  -DARROW_FILESYSTEM=OFF
  -DARROW_JEMALLOC=OFF
  -DARROW_MIMALLOC=OFF
  -DARROW_DEPENDENCY_SOURCE=BUNDLED
)
# Components that Parquet forces on are added here and recorded in docs/design/0003-arrow-boundary.md.
extra_flags=()
if [ -n "${QUARRY_ARROW_EXTRA_FLAGS:-}" ]; then
  read -r -a extra_flags <<<"$QUARRY_ARROW_EXTRA_FLAGS"
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/quarry-arrow.XXXXXX")"
cleanup() {
  if [ "${QUARRY_KEEP_ARROW_BUILD:-0}" = 1 ]; then
    echo "build_arrow: keeping $work" >&2
  else
    rm -rf "$work"
  fi
}
trap cleanup EXIT

fetch() { # <url> <file>
  curl --fail --silent --show-error --location --retry 3 --output "$2" "$1"
}

downloaded=0
for mirror in "${mirrors[@]}"; do
  rm -f "$work/$tarball" "$work/$tarball.sha512"
  if fetch "$mirror/$tarball" "$work/$tarball" &&
    fetch "$mirror/$tarball.sha512" "$work/$tarball.sha512"; then
    downloaded=1
    break
  fi
  echo "build_arrow: could not download the tarball and its checksum from $mirror" >&2
done
if [ "$downloaded" -ne 1 ]; then
  echo "build_arrow: no mirror served the tarball with its checksum" >&2
  exit 1
fi

expected="$(awk 'NF { print $1; exit }' "$work/$tarball.sha512")"
if ! [[ "$expected" =~ ^[0-9a-f]{128}$ ]]; then
  echo "build_arrow: the checksum file does not contain a SHA-512 digest" >&2
  exit 1
fi
if command -v sha512sum >/dev/null 2>&1; then
  actual="$(sha512sum "$work/$tarball" | awk '{ print $1 }')"
else
  actual="$(shasum -a 512 "$work/$tarball" | awk '{ print $1 }')"
fi
if [ "$expected" != "$actual" ]; then
  echo "build_arrow: SHA-512 mismatch for $tarball" >&2
  echo "  expected $expected" >&2
  echo "  actual   $actual" >&2
  exit 1
fi
echo "build_arrow: $tarball verified"

tar -xzf "$work/$tarball" -C "$work"
src="$work/apache-arrow-${version}/cpp"
[ -d "$src" ] || {
  echo "build_arrow: $src not found after extraction" >&2
  exit 1
}

mkdir -p "$prefix"
start="$(date +%s)"
CC="$cc" CXX="$cxx" cmake -S "$src" -B "$work/build" -G Ninja \
  "${flags[@]}" ${extra_flags[@]+"${extra_flags[@]}"} \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$work/build" --parallel "$jobs"
cmake --install "$work/build" --parallel "$jobs"
elapsed=$(($(date +%s) - start))

{
  echo "arrow_version: $version"
  echo "compiler: $("$cxx" --version 2>&1 | head -n 1)"
  echo "compiler_id: $id"
  echo "build_seconds: $elapsed"
  echo "flags:"
  for flag in "${flags[@]}" ${extra_flags[@]+"${extra_flags[@]}"}; do
    echo "  $flag"
  done
  echo "libraries:"
  find "$prefix" -name 'lib*.a' | sed "s|^$prefix/||" | sort | sed 's/^/  /'
} >"$prefix/quarry-arrow-build.txt"

echo "build_arrow: installed into $prefix in ${elapsed}s"
