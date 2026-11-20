#ifndef QUARRY_EXEC_ROW_COMPARATOR_H_
#define QUARRY_EXEC_ROW_COMPARATOR_H_

#include <arrow/array.h>
#include <arrow/result.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace quarry {

// One column of an ORDER BY: the values, the direction and where NULLs go.
struct SortColumn {
  std::shared_ptr<arrow::Array> array;
  bool ascending = true;
  bool nulls_first = false;
};

// Compares two rows of a set of equally long columns by the sort columns in order. NULLs go first
// or last as each column says, whichever the direction; DOUBLE NaN is larger than every number.
// Rows that are equal on every column compare equal.
class RowComparator {
 public:
  static arrow::Result<RowComparator> Make(const std::vector<SortColumn>& columns);

  // Negative when row a sorts before row b, positive when after, 0 when they tie.
  int Compare(uint32_t a, uint32_t b) const;

 private:
  struct Column;
  RowComparator(std::vector<Column> columns, std::vector<std::shared_ptr<arrow::Array>> owners);

  std::shared_ptr<std::vector<Column>> columns_;
  std::vector<std::shared_ptr<arrow::Array>> owners_;  // keep the arrays the columns point into
};

}  // namespace quarry

#endif  // QUARRY_EXEC_ROW_COMPARATOR_H_
