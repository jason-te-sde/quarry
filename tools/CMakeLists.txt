# Result formatting, shared by the command-line tool and the tests that compare results with
# DuckDB's.
quarry_add_library(quarry_output_format SOURCES output_format.cc LIBS quarry_expr quarry_types)
target_include_directories(quarry_output_format PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(QUARRY_BUILD_TOOLS)
  add_executable(quarry quarry_main.cc)
  target_link_libraries(quarry PRIVATE quarry_engine quarry_io quarry_output_format)
  quarry_set_warnings(quarry)
endif()
