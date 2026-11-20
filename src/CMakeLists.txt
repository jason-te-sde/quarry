# The layering, one static library per directory. An arrow in the comment means "links":
#   types <- vector <- expr
#   parser (no Arrow)
#   binder -> parser, types, expr
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
quarry_add_library(quarry_parser)
quarry_add_library(quarry_binder LIBS quarry_parser quarry_types quarry_expr)
quarry_add_library(quarry_plan LIBS quarry_expr)
quarry_add_library(quarry_planner LIBS quarry_binder quarry_plan)
quarry_add_library(quarry_io SOURCES io/parquet_scan.cc LIBS quarry_vector quarry::arrow)
quarry_add_library(quarry_exec LIBS quarry_plan quarry_expr quarry_io)
quarry_add_library(quarry_engine LIBS quarry_planner quarry_exec)
