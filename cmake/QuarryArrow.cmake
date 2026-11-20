# Finds the pinned Arrow and Parquet 26.0.1 that scripts/build_arrow.sh installs, and nothing else:
# no system or Homebrew Arrow is ever considered. Exposes the interface target quarry::arrow.
#
# QUARRY_ARROW_PREFIX defaults to $HOME/.local/opt/arrow-26.0.1-<compiler-id>, the directory the
# script installs into; the id is the same one the script computes.

set(QUARRY_ARROW_VERSION 26.0.1)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_quarry_compiler_name gcc)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  set(_quarry_compiler_name appleclang)
else()
  set(_quarry_compiler_name clang)
endif()
string(REGEX MATCH "^[0-9]+" _quarry_compiler_major "${CMAKE_CXX_COMPILER_VERSION}")
set(QUARRY_COMPILER_ID "${_quarry_compiler_name}-${_quarry_compiler_major}")

if(DEFINED ENV{HOME})
  set(_quarry_default_prefix
    "$ENV{HOME}/.local/opt/arrow-${QUARRY_ARROW_VERSION}-${QUARRY_COMPILER_ID}")
else()
  set(_quarry_default_prefix "")
endif()
set(QUARRY_ARROW_PREFIX "${_quarry_default_prefix}" CACHE PATH
  "Install prefix of Arrow ${QUARRY_ARROW_VERSION}, built by scripts/build_arrow.sh")

if(NOT QUARRY_ARROW_PREFIX OR NOT IS_DIRECTORY "${QUARRY_ARROW_PREFIX}")
  message(FATAL_ERROR
    "Arrow ${QUARRY_ARROW_VERSION} not found at '${QUARRY_ARROW_PREFIX}'. "
    "Run scripts/build_arrow.sh, or pass -DQUARRY_ARROW_PREFIX=<dir>.")
endif()

# The static libraries need the Thrift, Snappy and Zstd that the build bundled; they are installed
# into the same prefix, so one search path covers all of it.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG ON)
find_package(Arrow ${QUARRY_ARROW_VERSION} EXACT CONFIG REQUIRED
  PATHS "${QUARRY_ARROW_PREFIX}" NO_DEFAULT_PATH)
find_package(Parquet ${QUARRY_ARROW_VERSION} EXACT CONFIG REQUIRED
  PATHS "${QUARRY_ARROW_PREFIX}" NO_DEFAULT_PATH)

add_library(quarry_arrow INTERFACE)
add_library(quarry::arrow ALIAS quarry_arrow)
target_link_libraries(quarry_arrow INTERFACE Parquet::parquet_static Arrow::arrow_static)
