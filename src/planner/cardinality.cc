#include "planner/cardinality.h"

#include <arrow/scalar.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

#include "binder/catalog.h"
#include "types/decimal.h"

namespace quarry {
namespace {

constexpr double kEqualSelectivity = 0.1;
constexpr double kNotEqualSelectivity = 0.9;
constexpr double kRangeSelectivity = 0.3;
constexpr double kInPerValue = 0.1;
constexpr double kInCap = 0.5;
constexpr double kIsNullSelectivity = 0.1;
constexpr double kIsNotNullSelectivity = 0.9;
constexpr double kExtraKeyPairSelectivity = 0.1;
constexpr double kGroupFraction = 0.1;

double RowsOfTable(const LogicalScan& scan) {
  return scan.table == nullptr ? 0.0 : static_cast<double>(scan.table->row_count);
}

constexpr double kUnknownDomain = std::numeric_limits<double>::infinity();

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// The value of an integer scalar, or false for any other type.
bool IntegerValue(const arrow::Scalar& scalar, Int128* out) {
  if (!scalar.is_valid) return false;
  switch (scalar.type->id()) {
    case arrow::Type::INT8:
      *out = static_cast<const arrow::Int8Scalar&>(scalar).value;
      return true;
    case arrow::Type::INT16:
      *out = static_cast<const arrow::Int16Scalar&>(scalar).value;
      return true;
    case arrow::Type::INT32:
      *out = static_cast<const arrow::Int32Scalar&>(scalar).value;
      return true;
    case arrow::Type::INT64:
      *out = static_cast<const arrow::Int64Scalar&>(scalar).value;
      return true;
    case arrow::Type::UINT8:
      *out = static_cast<const arrow::UInt8Scalar&>(scalar).value;
      return true;
    case arrow::Type::UINT16:
      *out = static_cast<const arrow::UInt16Scalar&>(scalar).value;
      return true;
    case arrow::Type::UINT32:
      *out = static_cast<const arrow::UInt32Scalar&>(scalar).value;
      return true;
    case arrow::Type::UINT64:
      *out = static_cast<const arrow::UInt64Scalar&>(scalar).value;
      return true;
    default:
      return false;
  }
}

// `max - min + 1` of an integer column over every row group of every file of the table, which
// bounds its number of distinct values; kUnknownDomain when any row group lacks statistics for it
// or the column is not an integer.
double ScanColumnDomain(const LogicalScan& scan, int output_index) {
  const TableInfo* table = scan.table;
  if (table == nullptr || output_index < 0 ||
      static_cast<size_t>(output_index) >= scan.columns.size()) {
    return kUnknownDomain;
  }
  const int table_column = scan.columns[static_cast<size_t>(output_index)];
  if (table_column < 0 || static_cast<size_t>(table_column) >= table->columns.size()) {
    return kUnknownDomain;
  }
  const std::string name = Lower(table->columns[static_cast<size_t>(table_column)].name);
  bool any = false;
  Int128 low = 0;
  Int128 high = 0;
  for (const ParquetFileInfo& file : table->file_info) {
    if (file.schema == nullptr) return kUnknownDomain;
    int field = -1;
    for (int c = 0; c < file.schema->num_fields(); ++c) {
      if (Lower(file.schema->field(c)->name()) == name) {
        field = c;
        break;
      }
    }
    if (field < 0) return kUnknownDomain;
    for (const RowGroupStats& group : file.row_groups) {
      if (group.num_rows == 0) continue;
      if (static_cast<size_t>(field) >= group.columns.size() ||
          !group.columns[static_cast<size_t>(field)].has_value()) {
        return kUnknownDomain;
      }
      const ColumnMinMax& stats = *group.columns[static_cast<size_t>(field)];
      Int128 group_low = 0;
      Int128 group_high = 0;
      if (stats.min == nullptr || stats.max == nullptr || !IntegerValue(*stats.min, &group_low) ||
          !IntegerValue(*stats.max, &group_high)) {
        return kUnknownDomain;
      }
      if (!any || group_low < low) low = group_low;
      if (!any || group_high > high) high = group_high;
      any = true;
    }
  }
  if (!any || high < low) return kUnknownDomain;
  return static_cast<double>(high - low + 1);
}

// Where a column comes from: the row count of its base table and the bound on its distinct values.
struct KeyOrigin {
  double base_rows = 0;
  double domain = kUnknownDomain;
};

// The scan that output column `index` of `node` comes from, followed through filters, sorts,
// limits, joins and plain column projections; false when the column is computed or its origin is
// not a single scan.
bool TraceKey(const LogicalNode& node, int index, KeyOrigin* origin) {
  switch (node.kind) {
    case LogicalKind::kScan: {
      const auto& scan = static_cast<const LogicalScan&>(node);
      origin->base_rows = RowsOfTable(scan);
      origin->domain = ScanColumnDomain(scan, index);
      return true;
    }
    case LogicalKind::kFilter:
      return TraceKey(*static_cast<const LogicalFilter&>(node).child, index, origin);
    case LogicalKind::kSort:
      return TraceKey(*static_cast<const LogicalSort&>(node).child, index, origin);
    case LogicalKind::kLimit:
      return TraceKey(*static_cast<const LogicalLimit&>(node).child, index, origin);
    case LogicalKind::kProject: {
      const auto& project = static_cast<const LogicalProject&>(node);
      const BoundExpr& e = *project.exprs[static_cast<size_t>(index)];
      return e.kind == ExprKind::kColumn && TraceKey(*project.child, e.column_index, origin);
    }
    case LogicalKind::kAggregate: {
      const auto& aggregate = static_cast<const LogicalAggregate&>(node);
      if (static_cast<size_t>(index) >= aggregate.groups.size()) return false;
      const BoundExpr& e = *aggregate.groups[static_cast<size_t>(index)];
      return e.kind == ExprKind::kColumn && TraceKey(*aggregate.child, e.column_index, origin);
    }
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(node);
      const int left_width = static_cast<int>(join.left->output_types().size());
      return index < left_width ? TraceKey(*join.left, index, origin)
                                : TraceKey(*join.right, index - left_width, origin);
    }
    case LogicalKind::kCrossProduct: {
      int offset = 0;
      for (const LogicalPlan& child : static_cast<const LogicalCrossProduct&>(node).children) {
        const int width = static_cast<int>(child->output_types().size());
        if (index < offset + width) return TraceKey(*child, index - offset, origin);
        offset += width;
      }
      return false;
    }
  }
  return false;
}

// The first column reference of an expression, or -1.
int FirstColumn(const BoundExpr& e) {
  if (e.kind == ExprKind::kColumn) return e.column_index;
  for (const ExprPtr& child : e.children) {
    const int c = FirstColumn(*child);
    if (c >= 0) return c;
  }
  return -1;
}

}  // namespace

double EstimateSelectivity(const BoundExpr& e) {
  switch (e.kind) {
    case ExprKind::kBinary:
      switch (e.op) {
        case BinaryOp::kEq:
          return kEqualSelectivity;
        case BinaryOp::kNe:
          return kNotEqualSelectivity;
        case BinaryOp::kLt:
        case BinaryOp::kLe:
        case BinaryOp::kGt:
        case BinaryOp::kGe:
          return kRangeSelectivity;
        case BinaryOp::kAnd:
          return EstimateSelectivity(*e.children[0]) * EstimateSelectivity(*e.children[1]);
        case BinaryOp::kOr: {
          const double a = EstimateSelectivity(*e.children[0]);
          const double b = EstimateSelectivity(*e.children[1]);
          return a + b - a * b;
        }
        default:
          return 1.0;
      }
    case ExprKind::kNot:
      return 1.0 - EstimateSelectivity(*e.children[0]);
    case ExprKind::kBetween:
      return kRangeSelectivity;
    case ExprKind::kInList:
      return std::min(kInCap, kInPerValue * static_cast<double>(e.in_list.size()));
    case ExprKind::kIsNull:
      return e.negated ? kIsNotNullSelectivity : kIsNullSelectivity;
    case ExprKind::kLiteral:
      // A constant predicate keeps everything or nothing.
      if (e.literal == nullptr || !e.literal->is_valid) return 0.0;
      if (e.literal->type->id() == arrow::Type::BOOL) {
        return static_cast<const arrow::BooleanScalar&>(*e.literal).value ? 1.0 : 0.0;
      }
      return 1.0;
    default:
      return 1.0;
  }
}

double EstimateJoinRows(double left_rows, double right_rows, const std::vector<double>& divisors) {
  double rows = left_rows * right_rows;
  if (divisors.empty()) return rows;
  rows /= std::max(1.0, divisors[0]);
  for (size_t i = 1; i < divisors.size(); ++i) rows *= kExtraKeyPairSelectivity;
  return rows;
}

double KeyBaseRows(const LogicalNode& side, const BoundExpr& key) {
  const int column = FirstColumn(key);
  KeyOrigin origin;
  if (column >= 0 && TraceKey(side, column, &origin)) return origin.base_rows;
  return EstimateRows(side);
}

double KeyDomain(const LogicalNode& side, const BoundExpr& key) {
  const int column = FirstColumn(key);
  KeyOrigin origin;
  if (column >= 0 && TraceKey(side, column, &origin)) return origin.domain;
  return kUnknownDomain;
}

double JoinKeyDivisor(const LogicalNode& left, const BoundExpr& left_key, const LogicalNode& right,
                      const BoundExpr& right_key) {
  const double base = std::min(KeyBaseRows(left, left_key), KeyBaseRows(right, right_key));
  const double domain = std::max(KeyDomain(left, left_key), KeyDomain(right, right_key));
  return std::max(1.0, std::min(base, domain));
}

double EstimateRows(const LogicalNode& node) {
  switch (node.kind) {
    case LogicalKind::kScan: {
      const auto& scan = static_cast<const LogicalScan&>(node);
      double rows = RowsOfTable(scan);
      for (const ExprPtr& f : scan.filters) rows *= EstimateSelectivity(*f);
      return rows;
    }
    case LogicalKind::kFilter: {
      const auto& filter = static_cast<const LogicalFilter&>(node);
      return EstimateRows(*filter.child) * EstimateSelectivity(*filter.predicate);
    }
    case LogicalKind::kProject:
      return EstimateRows(*static_cast<const LogicalProject&>(node).child);
    case LogicalKind::kSort:
      return EstimateRows(*static_cast<const LogicalSort&>(node).child);
    case LogicalKind::kLimit: {
      const auto& limit = static_cast<const LogicalLimit&>(node);
      return std::min(EstimateRows(*limit.child), static_cast<double>(limit.n));
    }
    case LogicalKind::kAggregate: {
      const auto& aggregate = static_cast<const LogicalAggregate&>(node);
      if (aggregate.groups.empty()) return 1.0;
      return std::max(1.0, EstimateRows(*aggregate.child) * kGroupFraction);
    }
    case LogicalKind::kCrossProduct: {
      double rows = 1.0;
      for (const LogicalPlan& child : static_cast<const LogicalCrossProduct&>(node).children) {
        rows *= EstimateRows(*child);
      }
      return rows;
    }
    case LogicalKind::kJoin: {
      const auto& join = static_cast<const LogicalJoin&>(node);
      std::vector<double> bases;
      bases.reserve(join.left_keys.size());
      for (size_t i = 0; i < join.left_keys.size(); ++i) {
        bases.push_back(
            JoinKeyDivisor(*join.left, *join.left_keys[i], *join.right, *join.right_keys[i]));
      }
      double rows = EstimateJoinRows(EstimateRows(*join.left), EstimateRows(*join.right), bases);
      if (join.residual != nullptr) rows *= EstimateSelectivity(*join.residual);
      return rows;
    }
  }
  return 0.0;
}

}  // namespace quarry
