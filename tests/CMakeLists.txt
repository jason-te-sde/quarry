include(FetchContent)

# The script tests need bash and git. REQUIRED makes a machine without them fail here, at configure
# time, instead of silently running fewer tests.
find_program(QUARRY_BASH bash REQUIRED)
find_program(QUARRY_GIT git REQUIRED)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE
  SYSTEM)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# GoogleTest compiles with its own warning flags, and newer compilers have opinions about its
# sources. That is not our code to fix.
foreach(dependency gtest gtest_main gmock gmock_main)
  if(TARGET ${dependency})
    target_compile_options(${dependency} PRIVATE -w)
  endif()
endforeach()

quarry_add_test(parquet_metadata SOURCES unit/io/parquet_metadata_test.cc LIBS quarry_io)

# Tests of the repository's own check scripts. The objects test compiles two scratch files with
# the C++ compiler this build uses.
foreach(script hygiene layering objects)
  add_test(NAME ${script}_script
    COMMAND ${CMAKE_COMMAND} -E env "CXX=${CMAKE_CXX_COMPILER}"
      ${QUARRY_BASH} "${PROJECT_SOURCE_DIR}/tests/scripts/${script}_test.sh"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(${script}_script PROPERTIES TIMEOUT 120)
endforeach()
