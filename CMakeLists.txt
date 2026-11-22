cmake_minimum_required(VERSION 3.25)

project(quarry
  VERSION 0.1.1
  DESCRIPTION "A vectorized SQL query engine in C++20 over Apache Arrow and Parquet"
  LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(QuarryOptions)
include(QuarryWarnings)
include(QuarryTargets)
include(QuarryArrow)

if(QUARRY_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(src)

if(QUARRY_BUILD_TOOLS OR QUARRY_BUILD_TESTS)
  add_subdirectory(tools)
endif()

if(QUARRY_BUILD_TESTS)
  add_subdirectory(tests)
endif()

if(QUARRY_FUZZ)
  if(NOT QUARRY_BUILD_TESTS)
    message(FATAL_ERROR "QUARRY_FUZZ needs QUARRY_BUILD_TESTS=ON")
  endif()
  add_subdirectory(fuzz)
endif()
