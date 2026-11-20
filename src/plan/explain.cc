#include "plan/explain.h"

#include <cmath>
#include <string>
#include <vector>

namespace quarry {
namespace {

std::string Join(const std::vector<std::string>& parts, const char* separator = ", ") {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += separator;
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

std::string SortKeys(const std::vector<SortKey>& keys) {
  std::vector<std::string> parts;
  parts.reserve(keys.size());
  for (const SortKey& k : keys) {
    parts.push_back(k.expr->ToString() + (k.ascending ? " ASC" : " DESC") +
                    (k.nulls_first ? " NULLS FIRST" : " NULLS LAST"));
  }
  return Join(parts);
}

std::string Params(const PhysicalNode& node) {
  switch (node.kind) {
    case PhysicalKind::kScan: {
      const auto& scan = static_cast<const PhysicalScan&>(node);
      std::vector<std::string> names;
      for (const PhysicalColumn& c : scan.columns) names.push_back(c.name);
      std::string out = scan.table_name + "; cols=" + Join(names, ",");
      if (scan.predicate != nullptr) out += "; filter=" + scan.predicate->ToString();
      if (!scan.prune_conjuncts.empty()) {
        std::vector<std::string> conjuncts;
        for (const ExprPtr& c : scan.prune_conjuncts) conjuncts.push_back(c->ToString());
        out += "; prune=" + Join(conjuncts, " AND ");
      }
      return out;
    }
    case PhysicalKind::kFilter:
      return static_cast<const PhysicalFilter&>(node).predicate->ToString();
    case PhysicalKind::kProject: {
      const auto& project = static_cast<const PhysicalProject&>(node);
      std::vector<std::string> parts;
      for (size_t i = 0; i < project.exprs.size(); ++i) {
        parts.push_back(project.exprs[i]->ToString() + " AS " + project.names[i]);
      }
      return Join(parts);
    }
    case PhysicalKind::kHashJoin: {
      const auto& join = static_cast<const PhysicalHashJoin&>(node);
      std::string out =
          "probe_keys=" + ExprList(join.probe_keys) + "; build_keys=" + ExprList(join.build_keys);
      if (join.residual != nullptr) out += "; residual=" + join.residual->ToString();
      return out;
    }
    case PhysicalKind::kHashAggregate: {
      const auto& aggregate = static_cast<const PhysicalHashAggregate&>(node);
      std::vector<std::string> aggs;
      for (const AggregateExpr& a : aggregate.aggregates) {
        const std::string call =
            a.arg == nullptr ? std::string(AggFuncName(a.func))
                             : std::string(AggFuncName(a.func)) + "(" + a.arg->ToString() + ")";
        aggs.push_back(call + " -> " + a.result_type.ToString());
      }
      return "groups=[" + ExprList(aggregate.groups) + "]; aggs=[" + Join(aggs) + "]";
    }
    case PhysicalKind::kSort:
      return SortKeys(static_cast<const PhysicalSort&>(node).keys);
    case PhysicalKind::kTopN: {
      const auto& top = static_cast<const PhysicalTopN&>(node);
      return "n=" + std::to_string(top.n) + "; keys=" + SortKeys(top.keys);
    }
    case PhysicalKind::kLimit:
      return "n=" + std::to_string(static_cast<const PhysicalLimit&>(node).n);
  }
  return "";
}

void Render(const PhysicalNode& node, int depth, std::string* out) {
  out->append(static_cast<size_t>(depth) * 2, ' ');
  *out += std::string(PhysicalKindName(node.kind)) + " #" + std::to_string(node.id) + " [" +
          Params(node) + "] est=" + std::to_string(std::llround(node.estimated_rows)) + "\n";
  for (const PhysicalNode* child : node.children()) Render(*child, depth + 1, out);
}

}  // namespace

std::string Explain(const PhysicalNode& node) {
  std::string out;
  Render(node, 0, &out);
  return out;
}

}  // namespace quarry
