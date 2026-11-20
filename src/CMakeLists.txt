# The layering, one static library per directory. An arrow in the comment means "links":
#   types <- vector <- expr
#   parser (no Arrow)
#   binder -> parser, types, expr, io, plan
#   plan -> expr
#   planner -> binder, plan
#   io -> vector
#   exec -> plan, expr, io
#   engine -> planner, exec
# planner and exec never include each other; scripts/check-layering.sh enforces the direction.
# A library without sources is an interface library until its code arrives.

quarry_add_library(quarry_types
  SOURCES types/logical_type.cc types/decimal.cc types/date.cc types/type_rules.cc
  LIBS quarry::arrow)
quarry_add_library(quarry_vector
  SOURCES vector/selection.cc vector/gather.cc vector/batch.cc
  LIBS quarry_types quarry::arrow)
quarry_add_library(quarry_expr
  SOURCES expr/bound_expr.cc expr/eval.cc expr/eval_util.cc expr/eval_arith.cc
          expr/eval_compare.cc expr/eval_logic.cc expr/eval_cast.cc
  LIBS quarry_vector quarry_types)
quarry_add_library(quarry_parser SOURCES parser/ast.cc parser/lexer.cc parser/parser.cc)
quarry_add_library(quarry_plan SOURCES plan/logical.cc plan/physical.cc plan/explain.cc
  LIBS quarry_expr)
quarry_add_library(quarry_binder SOURCES binder/catalog.cc binder/binder.cc
  LIBS quarry_parser quarry_types quarry_expr quarry_io quarry_plan)
quarry_add_library(quarry_planner
  SOURCES planner/rules.cc planner/join_graph.cc planner/plan_util.cc planner/cardinality.cc
          planner/physical_planner.cc
  LIBS quarry_binder quarry_plan)
quarry_add_library(quarry_io SOURCES io/parquet_scan.cc LIBS quarry_vector quarry::arrow)
quarry_add_library(quarry_exec
  SOURCES exec/memory_budget.cc exec/hash.cc exec/key_encoding.cc exec/key_index.cc
          exec/join_hash_table.cc exec/agg_hash_table.cc exec/scan_op.cc exec/operator_util.cc
          exec/row_comparator.cc exec/filter_op.cc exec/project_op.cc exec/hash_join_op.cc
          exec/hash_agg_op.cc exec/sort_op.cc exec/topn_op.cc exec/limit_op.cc
          exec/build_operators.cc
  LIBS quarry_plan quarry_expr quarry_io)
quarry_add_library(quarry_engine SOURCES engine/engine.cc LIBS quarry_planner quarry_exec)
