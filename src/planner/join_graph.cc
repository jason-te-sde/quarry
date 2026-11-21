#include "planner/join_graph.h"

#include <arrow/status.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

#include "common/flaws.h"
#include "planner/cardinality.h"

namespace quarry {
namespace {

// One equality between two relations. The keys index their own relation's output.
struct Edge {
  size_t a = 0;
  size_t b = 0;
  ExprPtr key_a;
  ExprPtr key_b;
  double divisor = 1.0;  // JoinKeyDivisor of the two key columns
};

void ScanNames(const LogicalNode& node, std::vector<std::string>* out) {
  if (node.kind == LogicalKind::kScan) {
    out->push_back(static_cast<const LogicalScan&>(node).table_name);
    return;
  }
  for (const LogicalNode* child : ChildrenOf(node)) ScanNames(*child, out);
}

std::string RelationName(const LogicalNode& node) {
  std::vector<std::string> names;
  ScanNames(node, &names);
  if (names.size() == 1) return names[0];
  std::string out = "(";
  for (size_t i = 0; i < names.size(); ++i) out += (i == 0 ? "" : ", ") + names[i];
  return out + ")";
}

// "<line>:<column>" of the first table the relation reads, in the order of the SQL text.
std::string RelationPosition(const LogicalNode& node) {
  if (node.kind == LogicalKind::kScan) {
    const auto& scan = static_cast<const LogicalScan&>(node);
    return std::to_string(scan.line) + ":" + std::to_string(scan.column);
  }
  const std::vector<const LogicalNode*> children = ChildrenOf(node);
  return children.empty() ? "1:1" : RelationPosition(*children[0]);
}

// The relation a column of the concatenated output belongs to.
size_t RelationOf(const std::vector<size_t>& offsets, int column) {
  size_t r = 0;
  while (r + 1 < offsets.size() && static_cast<size_t>(column) >= offsets[r + 1]) ++r;
  return r;
}

// The one relation an expression uses, or false when it uses none or several.
bool SingleRelation(const BoundExpr& e, const std::vector<size_t>& offsets, size_t* relation) {
  std::vector<int> columns;
  CollectColumns(e, &columns);
  if (columns.empty()) return false;
  const size_t first = RelationOf(offsets, columns[0]);
  for (const int c : columns) {
    if (RelationOf(offsets, c) != first) return false;
  }
  *relation = first;
  return true;
}

void ShiftColumns(BoundExpr* e, int delta) {
  if (e->kind == ExprKind::kColumn) e->column_index += delta;
  for (ExprPtr& child : e->children) ShiftColumns(child.get(), delta);
}

}  // namespace

arrow::Result<JoinTree> BuildJoinTree(std::vector<LogicalPlan> relations,
                                      std::vector<ExprPtr> conjuncts, bool greedy) {
  const size_t n = relations.size();
  std::vector<size_t> offsets;
  std::vector<size_t> widths;
  size_t total_width = 0;
  for (const LogicalPlan& r : relations) {
    offsets.push_back(total_width);
    widths.push_back(r->output_types().size());
    total_width += widths.back();
  }

  // Edges, in conjunct order; everything else is left over.
  std::vector<Edge> edges;
  std::vector<ExprPtr> leftover;
  for (ExprPtr& c : conjuncts) {
    size_t a = 0;
    size_t b = 0;
    if (c->kind == ExprKind::kBinary && c->op == BinaryOp::kEq &&
        SingleRelation(*c->children[0], offsets, &a) &&
        SingleRelation(*c->children[1], offsets, &b) && a != b) {
      Edge edge;
      edge.a = a;
      edge.b = b;
      edge.key_a = std::move(c->children[0]);
      edge.key_b = std::move(c->children[1]);
      ShiftColumns(edge.key_a.get(), -static_cast<int>(offsets[a]));
      ShiftColumns(edge.key_b.get(), -static_cast<int>(offsets[b]));
      edge.divisor = JoinKeyDivisor(*relations[a], *edge.key_a, *relations[b], *edge.key_b);
      edges.push_back(std::move(edge));
    } else {
      leftover.push_back(std::move(c));
    }
  }

  // Every relation must be reachable from the first one.
  std::vector<bool> reached(n, false);
  reached[0] = true;
  for (bool grew = true; grew;) {
    grew = false;
    for (const Edge& e : edges) {
      if (reached[e.a] != reached[e.b]) {
        reached[e.a] = reached[e.b] = true;
        grew = true;
      }
    }
  }
  for (size_t r = 1; r < n; ++r) {
    if (!reached[r]) {
      return arrow::Status::Invalid(
          "planner error at " + RelationPosition(*relations[r]) +
          ": requires a cross product between " + RelationName(*relations[0]) + " and " +
          RelationName(*relations[r]) + ": add an equality predicate that joins them");
    }
  }

  // The order of the relations.
  std::vector<double> rows;
  rows.reserve(n);
  for (const LogicalPlan& r : relations) rows.push_back(EstimateRows(*r));
  std::vector<size_t> order;
  std::vector<bool> placed(n, false);
  size_t first = 0;
  if (greedy) {
    for (size_t r = 1; r < n; ++r) {
      if (rows[r] < rows[first]) first = r;
    }
  }
  order.push_back(first);
  placed[first] = true;
  double current_rows = rows[first];
  while (order.size() < n) {
    size_t best = n;
    double best_rows = std::numeric_limits<double>::infinity();
    for (size_t r = 0; r < n; ++r) {
      if (placed[r]) continue;
      std::vector<double> bases;
      for (const Edge& e : edges) {
        if ((e.a == r && placed[e.b]) || (e.b == r && placed[e.a])) bases.push_back(e.divisor);
      }
      if (bases.empty()) continue;  // not connected yet
      if (!greedy) {
        best = r;  // FROM order: the first connected one
        best_rows = EstimateJoinRows(current_rows, rows[r], bases);
        break;
      }
      const double estimate = EstimateJoinRows(current_rows, rows[r], bases);
      if (estimate < best_rows) {
        best = r;
        best_rows = estimate;
      }
    }
    // Reachability was checked above, so some unplaced relation touches the placed ones.
    order.push_back(best);
    placed[best] = true;
    current_rows = best_rows;
  }

  // The new position of each relation's first column.
  std::vector<size_t> new_offsets(n, 0);
  size_t position = 0;
  for (const size_t r : order) {
    new_offsets[r] = position;
    position += widths[r];
  }

  // Build the left-deep tree.
  std::fill(placed.begin(), placed.end(), false);
  LogicalPlan tree = std::move(relations[order[0]]);
  placed[order[0]] = true;
  for (size_t step = 1; step < n; ++step) {
    const size_t r = order[step];
    auto join = std::make_unique<LogicalJoin>();
    for (Edge& e : edges) {
      if (e.key_a == nullptr) continue;  // already used
      const bool a_new = e.a == r && placed[e.b];
      const bool b_new = e.b == r && placed[e.a];
      if (!a_new && !b_new) continue;
      ExprPtr placed_key = std::move(a_new ? e.key_b : e.key_a);
      ExprPtr new_key = std::move(a_new ? e.key_a : e.key_b);
      ShiftColumns(placed_key.get(), static_cast<int>(new_offsets[a_new ? e.b : e.a]));
      join->left_keys.push_back(std::move(placed_key));
      join->right_keys.push_back(std::move(new_key));
      e.key_a = nullptr;
      e.key_b = nullptr;
    }
    if constexpr (kFlawJoinOrderDropsPredicate) {
      // The defect: a relation reachable by two key pairs keeps only the first.
      if (greedy && join->left_keys.size() > 1) {
        join->left_keys.resize(1);
        join->right_keys.resize(1);
      }
    }
    join->left = std::move(tree);
    join->right = std::move(relations[r]);
    tree = std::move(join);
    placed[r] = true;
  }

  JoinTree result;
  result.map.assign(total_width, -1);
  for (size_t r = 0; r < n; ++r) {
    for (size_t j = 0; j < widths[r]; ++j) {
      result.map[offsets[r] + j] = static_cast<int>(new_offsets[r] + j);
    }
  }
  for (ExprPtr& c : leftover) RemapColumns(c.get(), result.map);
  result.leftover = std::move(leftover);
  result.plan = std::move(tree);
  return result;
}

}  // namespace quarry
