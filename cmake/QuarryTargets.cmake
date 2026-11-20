# The functions every quarry target is created through, so that src/, tests/, tools/, fuzz/ and
# bench/ all get the same warnings, flaw definition and include paths.

# quarry_set_warnings(<target>)
#
# The per-target hook for every quarry target. Applies the warning set, plus the QUARRY_FLAW
# definition when one is selected.
function(quarry_set_warnings target)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  target_compile_options(${target} PRIVATE ${QUARRY_WARNING_FLAGS})
  if(QUARRY_FLAW_DEFINITION)
    target_compile_definitions(${target} PRIVATE ${QUARRY_FLAW_DEFINITION})
  endif()
endfunction()

# quarry_add_library(<name> [SOURCES <files...>] [LIBS <targets...>])
#
# A static library whose sources may include internal headers as "io/foo.h": those live under src/
# and are visible to the library, to the libraries that link it and to tests. A library without
# sources is an interface library, which lets the layering exist before the code does.
function(quarry_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "usage: quarry_add_library(<name> [SOURCES <files...>] [LIBS <targets...>])")
  endif()
  if(ARG_SOURCES)
    add_library(${name} STATIC ${ARG_SOURCES})
    target_compile_features(${name} PUBLIC cxx_std_20)
    target_include_directories(${name} PUBLIC
      "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    if(ARG_LIBS)
      target_link_libraries(${name} PUBLIC ${ARG_LIBS})
    endif()
    quarry_set_warnings(${name})
  else()
    add_library(${name} INTERFACE)
    target_compile_features(${name} INTERFACE cxx_std_20)
    target_include_directories(${name} INTERFACE
      "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
    if(ARG_LIBS)
      target_link_libraries(${name} INTERFACE ${ARG_LIBS})
    endif()
  endif()
endfunction()

# quarry_add_test(<name> SOURCES <files...> LIBS <targets...>)
#
# Builds <name>_test into <build>/tests/ and registers it with CTest. Needs GoogleTest, which
# tests/CMakeLists.txt fetches when QUARRY_BUILD_TESTS is ON.
function(quarry_add_test name)
  cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
  if(NOT ARG_SOURCES OR ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "usage: quarry_add_test(<name> SOURCES <files...> LIBS <targets...>)")
  endif()
  if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "quarry_add_test(${name}) needs GoogleTest; configure with QUARRY_BUILD_TESTS=ON")
  endif()
  set(target ${name}_test)
  add_executable(${target} ${ARG_SOURCES})
  target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(${target} PRIVATE ${ARG_LIBS} GTest::gtest_main)
  set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tests")
  quarry_set_warnings(${target})
  add_test(NAME ${target} COMMAND ${target} WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(${target} PROPERTIES TIMEOUT 300)
endfunction()
