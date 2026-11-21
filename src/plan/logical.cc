#include "plan/logical.h"

#include <utility>

namespace quarry {
namespace {

std::string Pad(int indent) { return std::string(static_cast<size_t>(indent) * 2, ' '); }

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += ", ";
    out += parts[i];
  }
  return out;
}

std::string ExprList(const std::vector<ExprPtr>& exprs) {
  std::vector<std::string> parts;
  parts.reserve(exprs.size());
  for (const ExprPtr& e : exprs) parts.push_back(e->ToString());
  return Join(parts);
}

}  // namespace

std::vector<LogicalType> LogicalScan::output_types() const {
  std::vector<LogicalType> types;
  types.reserve(columns.size());
  for (const int c : columns) {
    // A column of an unsupported type is never listed; an unlisted type here is a binder bug, and
    // an INTEGER placeholder keeps the answer well formed.
    types.push_back(table_column_types[static_cast<size_t>(c)].value_or(LogicalType::Int32()));
  }
  return types;
}

std::string LogicalScan::ToString(int indent) const {
  std::vector<std::string> names;
  names.reserve(columns.size());
  for (const int c : columns) names.push_back(table_column_names[static_cast<size_t>(c)]);
  std::string out = Pad(indent) + "Scan " + table_name + " [" + Join(names) + "]";
  if (!filters.empty()) out += " filters=[" + ExprList(filters) + "]";
  return out + "\n";
}

std::vector<LogicalType> LogicalFilter::output_types() const { return child->output_types(); }

std::string LogicalFilter::ToString(int indent) const {
  return Pad(indent) + "Filter " + predicate->ToString() + "\n" + child->ToString(indent + 1);
}

std::vector<LogicalType> LogicalProject::output_types() const {
  std::vector<LogicalType> types;
  types.reserve(exprs.size());
  for (const ExprPtr& e : exprs) types.push_back(e->type);
  return types;
}

std::string LogicalProject::ToString(int indent) const {
  std::vector<std::string> parts;
  parts.reserve(exprs.size());
  for (size_t i = 0; i < exprs.size(); ++i) {
    parts.push_back(exprs[i]->ToString() + " AS " + names[i]);
  }
  return Pad(indent) + "Project [" + Join(parts) + "]\n" + child->ToString(indent + 1);
}

std::vector<LogicalType> LogicalCrossProduct::output_types() const {
  std::vector<LogicalType> types;
  for (const LogicalPlan& c : children) {
    const std::vector<LogicalType> child_types = c->output_types();
    types.insert(types.end(), child_types.begin(), child_types.end());
  }
  return types;
}

std::string LogicalCrossProduct::ToString(int indent) const {
  std::string out = Pad(indent) + "CrossProduct\n";
  for (const LogicalPlan& c : children) out += c->ToString(indent + 1);
  return out;
}

std::vector<LogicalType> LogicalJoin::output_types() const {
  std::vector<LogicalType> types = left->output_types();
  const std::vector<LogicalType> right_types = right->output_types();
  types.insert(types.end(), right_types.begin(), right_types.end());
  return types;
}

std::string LogicalJoin::ToString(int indent) const {
  std::vector<std::string> keys;
  keys.reserve(left_keys.size());
  for (size_t i = 0; i < left_keys.size(); ++i) {
    keys.push_back("(" + left_keys[i]->ToString() + " = " + right_keys[i]->ToString() + ")");
  }
  std::string out = Pad(indent) + "Join keys=[" + Join(keys) + "]";
  if (residual != nullptr) out += " residual=" + residual->ToString();
  out += "\n" + left->ToString(indent + 1) + right->ToString(indent + 1);
  return out;
}

std::vector<LogicalType> LogicalAggregate::output_types() const {
  std::vector<LogicalType> types;
  types.reserve(groups.size() + aggregates.size());
  for (const ExprPtr& g : groups) types.push_back(g->type);
  for (const AggregateExpr& a : aggregates) types.push_back(a.result_type);
  return types;
}

std::string LogicalAggregate::ToString(int indent) const {
  std::vector<std::string> aggs;
  for (const AggregateExpr& a : aggregates) {
    // AggFuncName(kCountStar) is "COUNT(*)" already.
    const std::string call = a.arg == nullptr
                                 ? std::string(AggFuncName(a.func))
                                 : std::string(AggFuncName(a.func)) + "(" + a.arg->ToString() + ")";
    aggs.push_back(call + " -> " + a.result_type.ToString());
  }
  return Pad(indent) + "Aggregate groups=[" + ExprList(groups) + "] aggs=[" + Join(aggs) + "]\n" +
         child->ToString(indent + 1);
}

std::vector<LogicalType> LogicalSort::output_types() const { return child->output_types(); }

std::string LogicalSort::ToString(int indent) const {
  std::vector<std::string> parts;
  parts.reserve(keys.size());
  for (const SortKey& k : keys) {
    parts.push_back(k.expr->ToString() + (k.ascending ? " ASC" : " DESC") +
                    (k.nulls_first ? " NULLS FIRST" : " NULLS LAST"));
  }
  return Pad(indent) + "Sort [" + Join(parts) + "]\n" + child->ToString(indent + 1);
}

std::vector<LogicalType> LogicalLimit::output_types() const { return child->output_types(); }

std::string LogicalLimit::ToString(int indent) const {
  return Pad(indent) + "Limit " + std::to_string(n) + "\n" + child->ToString(indent + 1);
}

}  // namespace quarry
