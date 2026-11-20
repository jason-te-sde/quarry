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

quarry_add_test(Lexer SOURCES unit/parser/lexer_test.cc LIBS quarry_parser)
quarry_add_test(Parser SOURCES unit/parser/parser_test.cc LIBS quarry_parser)
quarry_add_test(Catalog SOURCES unit/binder/catalog_test.cc LIBS quarry_binder quarry_test_fixtures)
quarry_add_test(Binder SOURCES unit/binder/binder_test.cc LIBS quarry_binder quarry_test_fixtures)
quarry_add_test(parquet_metadata SOURCES unit/io/parquet_metadata_test.cc LIBS quarry_io)

# Test-only support: Values, and the row-at-a-time reference evaluator the property test compares
# the vectorized evaluator against.
quarry_add_library(quarry_test_support
  SOURCES support/value.cc support/reference_eval.cc
  LIBS quarry_expr)
target_include_directories(quarry_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# Writes small Parquet files (of types the engine does and does not support) for the catalog and
# binder tests.
quarry_add_library(quarry_test_fixtures
  SOURCES support/parquet_fixture.cc
  LIBS quarry::arrow GTest::gtest)
target_include_directories(quarry_test_fixtures PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

# The names start with a capital so that `ctest -R 'Type|Decimal|Date|Selection|Eval'` finds them.
quarry_add_test(Decimal SOURCES unit/types/decimal_test.cc LIBS quarry_types)
quarry_add_test(Date SOURCES unit/types/date_test.cc LIBS quarry_types)
quarry_add_test(TypeRules SOURCES unit/types/type_rules_test.cc LIBS quarry_types)
quarry_add_test(Selection SOURCES unit/vector/selection_test.cc LIBS quarry_vector quarry_test_support)
quarry_add_test(Eval SOURCES unit/expr/eval_test.cc LIBS quarry_expr quarry_test_support)
quarry_add_test(EvalProperty SOURCES unit/expr/eval_property_test.cc LIBS quarry_expr quarry_test_support)

# SQL error tests: tests/sql/*.test, run by one program against the TPC-H test data.
quarry_add_test(SqlRunner SOURCES sql/sql_test_runner.cc LIBS quarry_binder)

# The optimizer: rules, join order, cardinality, physical plan, and the plan goldens.
quarry_add_test(Rules SOURCES unit/planner/rules_test.cc LIBS quarry_planner quarry_test_fixtures)
quarry_add_test(JoinOrder SOURCES unit/planner/join_order_test.cc LIBS quarry_planner quarry_test_fixtures)
quarry_add_test(Cardinality SOURCES unit/planner/cardinality_test.cc LIBS quarry_planner)
quarry_add_test(PhysicalPlanner SOURCES unit/planner/physical_planner_test.cc LIBS quarry_planner quarry_test_fixtures)
quarry_add_test(PlanGolden SOURCES planner/plan_golden_test.cc LIBS quarry_planner quarry_test_fixtures)
# Rewriting goldens under CI must fail, not rewrite: run the golden test that way and expect it to
# fail, with the golden files left alone.
add_test(NAME PlanGolden_refuses_update_in_ci
  COMMAND ${CMAKE_COMMAND} -E env UPDATE_GOLDENS=1 CI=1 $<TARGET_FILE:PlanGolden_test>
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
set_tests_properties(PlanGolden_refuses_update_in_ci PROPERTIES WILL_FAIL TRUE TIMEOUT 300)

# Tests of the repository's own check scripts. The objects test compiles two scratch files with
# the C++ compiler this build uses.
foreach(script hygiene layering objects)
  add_test(NAME ${script}_script
    COMMAND ${CMAKE_COMMAND} -E env "CXX=${CMAKE_CXX_COMPILER}"
      ${QUARRY_BASH} "${PROJECT_SOURCE_DIR}/tests/scripts/${script}_test.sh"
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
  set_tests_properties(${script}_script PROPERTIES TIMEOUT 120)
endforeach()

# Operators and their hash tables.
quarry_add_test(MemoryBudget SOURCES unit/exec/memory_budget_test.cc LIBS quarry_exec)
quarry_add_test(JoinHashTable SOURCES unit/exec/join_hash_table_test.cc LIBS quarry_exec)
quarry_add_test(AggHashTable SOURCES unit/exec/agg_hash_table_test.cc LIBS quarry_exec quarry_test_support)
