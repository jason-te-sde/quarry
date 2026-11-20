#include "exec/row_comparator.h"

#include <arrow/status.h>

#include <cmath>
#include <cstring>
#include <string_view>

#include "expr/eval_internal.h"
#include "types/decimal.h"

namespace quarry {

struct RowComparator::Column {
  arrow::Type::type id = arrow::Type::NA;
  const arrow::Array* array = nullptr;
  const uint8_t* values = nullptr;
  const uint8_t* valid = nullptr;  // null when the column has no NULL
  const int32_t* string_offsets = nullptr;
  const uint8_t* string_bytes = nullptr;
  int64_t offset = 0;
  bool ascending = true;
  bool nulls_first = false;
};

namespace {

using eval_internal::BitAt;
using eval_internal::Load;

template <typename T>
int Three(T a, T b) {
  return a < b ? -1 : (a > b ? 1 : 0);
}

int CompareDoubles(double a, double b) {
  const bool a_nan = std::isnan(a);
  const bool b_nan = std::isnan(b);
  if (a_nan || b_nan) return a_nan == b_nan ? 0 : (a_nan ? 1 : -1);
  return Three(a, b);
}

}  // namespace

RowComparator::RowComparator(std::vector<Column> columns,
                             std::vector<std::shared_ptr<arrow::Array>> owners)
    : columns_(std::make_shared<std::vector<Column>>(std::move(columns))),
      owners_(std::move(owners)) {}

arrow::Result<RowComparator> RowComparator::Make(std::vector<SortColumn> columns) {
  std::vector<Column> out;
  std::vector<std::shared_ptr<arrow::Array>> owners;
  for (const SortColumn& c : columns) {
    owners.push_back(c.array);
    Column column;
    column.array = c.array.get();
    column.id = c.array->type_id();
    column.offset = c.array->offset();
    column.ascending = c.ascending;
    column.nulls_first = c.nulls_first;
    column.valid = c.array->null_count() > 0 ? c.array->null_bitmap_data() : nullptr;
    switch (column.id) {
      case arrow::Type::BOOL:
      case arrow::Type::INT32:
      case arrow::Type::DATE32:
      case arrow::Type::INT64:
      case arrow::Type::DOUBLE:
      case arrow::Type::DECIMAL64:
      case arrow::Type::DECIMAL128:
        column.values = eval_internal::ValueBytes(*c.array);
        break;
      case arrow::Type::STRING:
        column.string_offsets =
            reinterpret_cast<const int32_t*>(c.array->data()->buffers[1]->data()) + column.offset;
        column.string_bytes =
            c.array->data()->buffers[2] != nullptr ? c.array->data()->buffers[2]->data() : nullptr;
        break;
      default:
        return arrow::Status::Invalid("cannot sort by a column of type ",
                                      c.array->type()->ToString());
    }
    out.push_back(column);
  }
  return RowComparator(std::move(out), std::move(owners));
}

int RowComparator::Compare(uint32_t a, uint32_t b) const {
  for (const Column& c : *columns_) {
    const bool a_null = c.valid != nullptr && !BitAt(c.valid, c.offset + a);
    const bool b_null = c.valid != nullptr && !BitAt(c.valid, c.offset + b);
    if (a_null || b_null) {
      if (a_null && b_null) continue;
      return (a_null == c.nulls_first) ? -1 : 1;
    }
    int result = 0;
    switch (c.id) {
      case arrow::Type::BOOL:
        result = Three(BitAt(c.values, c.offset + a), BitAt(c.values, c.offset + b));
        break;
      case arrow::Type::INT32:
      case arrow::Type::DATE32:
        result = Three(Load<int32_t>(c.values, c.offset, a), Load<int32_t>(c.values, c.offset, b));
        break;
      case arrow::Type::INT64:
      case arrow::Type::DECIMAL64:
        result = Three(Load<int64_t>(c.values, c.offset, a), Load<int64_t>(c.values, c.offset, b));
        break;
      case arrow::Type::DOUBLE:
        result = CompareDoubles(Load<double>(c.values, c.offset, a),
                                Load<double>(c.values, c.offset, b));
        break;
      case arrow::Type::DECIMAL128:
        result = Three(Load<Int128>(c.values, c.offset, a), Load<Int128>(c.values, c.offset, b));
        break;
      case arrow::Type::STRING: {
        const std::string_view x(
            reinterpret_cast<const char*>(c.string_bytes) + c.string_offsets[a],
            static_cast<size_t>(c.string_offsets[a + 1] - c.string_offsets[a]));
        const std::string_view y(
            reinterpret_cast<const char*>(c.string_bytes) + c.string_offsets[b],
            static_cast<size_t>(c.string_offsets[b + 1] - c.string_offsets[b]));
        result = x.compare(y);
        result = result < 0 ? -1 : (result > 0 ? 1 : 0);
        break;
      }
      default:
        break;
    }
    if (result != 0) return c.ascending ? result : -result;
  }
  return 0;
}

}  // namespace quarry
