# Build options, toolchain floors, and the flags that must reach every target (sanitizers,
# coverage). Included once from the top-level CMakeLists.txt, so everything below runs in that
# directory scope and applies to every target created afterwards, fetched dependencies included.

option(QUARRY_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(QUARRY_BUILD_TOOLS "Build the command-line tools" ON)
option(QUARRY_BUILD_BENCH "Build the benchmarks" OFF)
option(QUARRY_FUZZ "Build the fuzz targets (Clang only)" OFF)
option(QUARRY_COVERAGE "Instrument the build for coverage" OFF)

set(QUARRY_SANITIZE "" CACHE STRING "Sanitizers to enable: empty or \"address,undefined\"")
set_property(CACHE QUARRY_SANITIZE PROPERTY STRINGS "" "address,undefined")

# One deliberate flaw can be compiled in so that the test suite can prove it would notice the bug.
# The name is lower-case; it becomes the compile definition QUARRY_FLAW_<UPPER_NAME>=1, which only
# src/common/flaws.h reads.
set(QUARRY_FLAW "" CACHE STRING "Compile in one deliberate flaw: empty or a lower-case flaw name")

# Compiler floors: GCC 14, Clang 18, AppleClang 15.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_quarry_compiler_floor 14)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(_quarry_compiler_floor 18)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  set(_quarry_compiler_floor 15)
else()
  message(FATAL_ERROR
    "Unsupported compiler '${CMAKE_CXX_COMPILER_ID}'; quarry needs GCC, Clang or AppleClang")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS _quarry_compiler_floor)
  message(FATAL_ERROR
    "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} is too old; "
    "quarry needs ${CMAKE_CXX_COMPILER_ID} ${_quarry_compiler_floor} or newer")
endif()

if(QUARRY_FUZZ AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "QUARRY_FUZZ needs Clang (libFuzzer), but the compiler is ${CMAKE_CXX_COMPILER_ID}")
endif()
# A fuzz target without AddressSanitizer would find only the inputs that happen to crash.
if(QUARRY_FUZZ AND NOT QUARRY_SANITIZE STREQUAL "address,undefined")
  message(FATAL_ERROR
    "QUARRY_FUZZ needs QUARRY_SANITIZE=address,undefined, but it is '${QUARRY_SANITIZE}'; "
    "the fuzz preset sets both")
endif()

# Sanitizers and coverage change the runtime, so they apply to fetched dependencies too.
if(NOT QUARRY_SANITIZE STREQUAL "")
  if(NOT QUARRY_SANITIZE STREQUAL "address,undefined")
    message(FATAL_ERROR "QUARRY_SANITIZE='${QUARRY_SANITIZE}' is not one of: '', 'address,undefined'")
  endif()
  add_compile_options(-fsanitize=${QUARRY_SANITIZE} -fno-omit-frame-pointer
    -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${QUARRY_SANITIZE} -fno-sanitize-recover=all)
endif()

# libFuzzer steers by the coverage of the code under test, so the engine libraries need the
# instrumentation too; "-no-link" adds the counters without the libFuzzer runtime.
if(QUARRY_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link -O1)
endif()

if(QUARRY_COVERAGE)
  add_compile_options(--coverage -fprofile-update=atomic)
  add_link_options(--coverage)
endif()

if(NOT QUARRY_FLAW STREQUAL "")
  if(NOT QUARRY_FLAW MATCHES "^[a-z0-9_]+$")
    message(FATAL_ERROR "QUARRY_FLAW='${QUARRY_FLAW}' must be lower-case letters, digits and '_'")
  endif()
  string(TOUPPER "${QUARRY_FLAW}" _quarry_flaw_upper)
  set(QUARRY_FLAW_DEFINITION "QUARRY_FLAW_${_quarry_flaw_upper}=1")
endif()
