#include "exec/scan_op.h"

#include <arrow/io/file.h>
#include <arrow/scalar.h>
#include <arrow/status.h>
#include <arrow/util/decimal.h>
#include <parquet/arrow/reader.h>
#include <parquet/exception.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#include "common/flaws.h"
#include "exec/operator_util.h"
#include "expr/eval.h"
#include "expr/eval_internal.h"
#include "io/parquet_scan.h"
#include "types/decimal.h"

namespace quarry {
namespace {

std::string Lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

// A value of a column or a literal in a form that can be ordered: an integer scaled by 10^scale.
// Numbers of different kinds (a date and a decimal) are never compared.
struct Orderable {
  enum class Kind : uint8_t { kNone, kInteger, kDecimal, kDate };
  Kind kind = Kind::kNone;
  Int128 value = 0;
  int scale = 0;
};

Orderable ToOrderable(const arrow::Scalar& scalar) {
  Orderable out;
  if (!scalar.is_valid) return out;
  switch (scalar.type->id()) {
    case arrow::Type::INT32:
      out = {Orderable::Kind::kInteger, static_cast<const arrow::Int32Scalar&>(scalar).value, 0};
      break;
    case arrow::Type::INT64:
      out = {Orderable::Kind::kInteger, static_cast<const arrow::Int64Scalar&>(scalar).value, 0};
      break;
    case arrow::Type::DATE32:
      out = {Orderable::Kind::kDate, static_cast<const arrow::Date32Scalar&>(scalar).value, 0};
      break;
    case arrow::Type::DECIMAL64: {
      const auto& decimal = static_cast<const arrow::Decimal64Scalar&>(scalar);
      out = {Orderable::Kind::kDecimal, static_cast<int64_t>(decimal.value),
             static_cast<const arrow::Decimal64Type&>(*scalar.type).scale()};
      break;
    }
    case arrow::Type::DECIMAL128: {
      const auto& decimal = static_cast<const arrow::Decimal128Scalar&>(scalar);
      Int128 value;
      static_assert(sizeof(value) == 16);
      const auto bytes = decimal.value.ToBytes();
      std::memcpy(&value, bytes.data(), sizeof(value));
      out = {Orderable::Kind::kDecimal, value,
             static_cast<const arrow::Decimal128Type&>(*scalar.type).scale()};
      break;
    }
    default:
      break;
  }
  return out;
}

// -1, 0 or 1; false when the two cannot be compared.
bool Compare(const Orderable& a, const Orderable& b, int* result) {
  if (a.kind == Orderable::Kind::kNone || a.kind != b.kind) return false;
  Int128 x = a.value;
  Int128 y = b.value;
  if (a.scale < b.scale) {
    if (__builtin_mul_overflow(x, PowerOfTen<Int128>(b.scale - a.scale), &x)) return false;
  } else if (b.scale < a.scale) {
    if (__builtin_mul_overflow(y, PowerOfTen<Int128>(a.scale - b.scale), &y)) return false;
  }
  *result = x < y ? -1 : (x > y ? 1 : 0);
  return true;
}

// The maximum of a row group is below the literal. The row group then cannot hold a row that is
// equal to or above the literal.
bool MaxBelow(int max_vs_literal) {
  if constexpr (kFlawPruneWrongBound) {
    return max_vs_literal <= 0;  // the defect: a maximum equal to the literal counts as below
  }
  return max_vs_literal < 0;
}

// False when the conjunct cannot hold for any row of a row group whose column has these minimum
// and maximum; true when it may, or when the statistics cannot say.
bool MayMatch(const BoundExpr& conjunct, const ColumnMinMax& stats) {
  const Orderable min = ToOrderable(*stats.min);
  const Orderable max = ToOrderable(*stats.max);
  int min_vs = 0;  // min compared with the literal
  int max_vs = 0;
  if (conjunct.kind == ExprKind::kBinary) {
    const Orderable literal = ToOrderable(*conjunct.children[1]->literal);
    if (!Compare(min, literal, &min_vs) || !Compare(max, literal, &max_vs)) return true;
    switch (conjunct.op) {
      case BinaryOp::kEq:
        return !(min_vs > 0 || MaxBelow(max_vs));
      case BinaryOp::kLt:
        return !(min_vs >= 0);
      case BinaryOp::kLe:
        return !(min_vs > 0);
      case BinaryOp::kGt:
        return !(max_vs <= 0);
      case BinaryOp::kGe:
        return !MaxBelow(max_vs);
      default:
        return true;
    }
  }
  if (conjunct.kind == ExprKind::kBetween) {
    const Orderable low = ToOrderable(*conjunct.children[1]->literal);
    const Orderable high = ToOrderable(*conjunct.children[2]->literal);
    int min_vs_high = 0;
    int max_vs_low = 0;
    if (!Compare(min, high, &min_vs_high) || !Compare(max, low, &max_vs_low)) return true;
    return !(min_vs_high > 0 || MaxBelow(max_vs_low));
  }
  return true;
}

int64_t BufferBytes(const arrow::Array& array) {
  int64_t bytes = 0;
  for (const auto& buffer : array.data()->buffers) {
    if (buffer != nullptr) bytes += buffer->size();
  }
  return bytes;
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> SingleChunk(
    const std::shared_ptr<arrow::ChunkedArray>& column, const std::string& name) {
  if (column->num_chunks() == 1) return column->chunk(0);
  if (column->num_chunks() == 0) return arrow::MakeEmptyArray(column->type());
  return arrow::Status::Invalid("column ", name, " has ", column->num_chunks(),
                                " chunks in one row group and cannot be read as one array");
}

struct ScanOperator::FileState {
  std::filesystem::path path;
  ParquetFileInfo info;
  std::vector<int> file_columns;        // Parquet leaf of each output column; the first when none
  std::vector<int> prune_file_columns;  // Arrow field of each prune conjunct's column, or -1
  std::vector<int> groups;              // row groups that survive pruning
  size_t next_group = 0;
  std::unique_ptr<parquet::arrow::FileReader> reader;
};

ScanOperator::ScanOperator(const PhysicalScan& node) : node_(node) {}

ScanOperator::~ScanOperator() = default;

arrow::Status ScanOperator::Open(ExecContext& context) {
  context_ = &context;
  files_.clear();
  report_ = ScanReport();
  file_ = 0;
  row_group_ = -1;
  columns_.clear();
  group_rows_ = 0;
  position_ = 0;
  scan_buffer_bytes_ = 0;
  for (size_t f = 0; f < node_.files.size(); ++f) {
    ARROW_RETURN_NOT_OK(OpenFile(f));
  }
  report_.columns_decoded = node_.columns.empty() ? 1 : static_cast<int>(node_.columns.size());
  return arrow::Status::OK();
}

arrow::Status ScanOperator::OpenFile(size_t index) {
  auto state = std::make_unique<FileState>();
  state->path = node_.files[index];
  ARROW_ASSIGN_OR_RAISE(state->info, ReadParquetMetadata(state->path));
  const ParquetFileInfo& info = state->info;

  const auto column_of = [&](const std::string& name) -> int {
    for (int c = 0; c < info.schema->num_fields(); ++c) {
      if (Lower(info.schema->field(c)->name()) == name) return c;
    }
    return -1;
  };
  // Arrow fields and Parquet leaves are numbered differently after a nested field; the reader
  // takes leaves, the statistics are per field.
  std::vector<int> field_of_column;
  for (const PhysicalColumn& column : node_.columns) {
    const int c = column_of(column.name);
    if (c < 0) {
      return arrow::Status::Invalid(state->path.string(), " has no column ", column.name);
    }
    const int leaf = info.field_leaf[static_cast<size_t>(c)];
    if (leaf < 0) {
      return arrow::Status::Invalid(state->path.string(), ": column ", column.name,
                                    " is nested and cannot be read");
    }
    field_of_column.push_back(c);
    state->file_columns.push_back(leaf);
  }
  if (state->file_columns.empty() && info.num_leaves > 0) {
    state->file_columns.push_back(0);  // a scan that needs no column still has to count rows
  }
  for (const ExprPtr& conjunct : node_.prune_conjuncts) {
    const BoundExpr& column = *conjunct->children[0];
    state->prune_file_columns.push_back(
        column.column_index >= 0 && static_cast<size_t>(column.column_index) < node_.columns.size()
            ? field_of_column[static_cast<size_t>(column.column_index)]
            : -1);
  }

  report_.row_groups_total += info.num_row_groups;
  for (int g = 0; g < info.num_row_groups; ++g) {
    bool may_match = info.row_groups[static_cast<size_t>(g)].num_rows > 0;
    for (size_t k = 0; may_match && k < node_.prune_conjuncts.size(); ++k) {
      const int c = state->prune_file_columns[k];
      if (c < 0) continue;
      const auto& stats = info.row_groups[static_cast<size_t>(g)].columns[static_cast<size_t>(c)];
      if (stats.has_value() && !MayMatch(*node_.prune_conjuncts[k], *stats)) may_match = false;
    }
    if (may_match) state->groups.push_back(g);
  }
  files_.push_back(std::move(state));
  return arrow::Status::OK();
}

arrow::Result<bool> ScanOperator::LoadNextRowGroup() {
  while (file_ < files_.size()) {
    ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
    FileState& file = *files_[file_];
    if (file.next_group >= file.groups.size()) {
      file.reader.reset();
      ++file_;
      continue;
    }
    const int group = file.groups[file.next_group++];
    std::shared_ptr<arrow::Table> table;
    try {
      if (file.reader == nullptr) {
        ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(file.path.string()));
        ARROW_ASSIGN_OR_RAISE(file.reader, parquet::arrow::OpenFile(input, ScanPool()));
      }
      auto read = file.reader->ReadRowGroup(group, file.file_columns);
      if (!read.ok()) {
        return read.status().WithMessage(file.path.string() + ": " + read.status().message());
      }
      table = std::move(*read);
    } catch (const parquet::ParquetStatusException& e) {
      return e.status().WithMessage(file.path.string() + ": " + e.status().message());
    } catch (const parquet::ParquetException& e) {
      return arrow::Status::IOError(file.path.string(), ": ", e.what());
    }
    report_.row_groups_read.emplace_back(static_cast<int>(file_), group);
    ARROW_ASSIGN_OR_RAISE(table, table->CombineChunks(ScanPool()));
    group_rows_ = table->num_rows();
    position_ = 0;
    columns_.clear();
    int64_t buffer_bytes = 0;
    for (size_t i = 0; i < node_.columns.size(); ++i) {
      const std::shared_ptr<arrow::ChunkedArray>& chunked = table->column(static_cast<int>(i));
      auto single = SingleChunk(chunked, node_.columns[i].name);
      if (!single.ok()) {
        return single.status().WithMessage(file.path.string() + ": " + single.status().message());
      }
      std::shared_ptr<arrow::Array> array = std::move(*single);
      const LogicalType& type = node_.columns[i].type;
      buffer_bytes += BufferBytes(*array);
      if (type.is_decimal64() && array->type_id() == arrow::Type::DECIMAL128) {
        ARROW_ASSIGN_OR_RAISE(array, eval_internal::NarrowDecimal128(*array, ScanPool()));
      }
      if (!array->type()->Equals(*type.ToArrow())) {
        return arrow::Status::Invalid(file.path.string(), ": column ", node_.columns[i].name,
                                      " has type ", array->type()->ToString(), ", expected ",
                                      type.ToArrow()->ToString());
      }
      columns_.push_back(std::move(array));
    }
    scan_buffer_bytes_ = std::max(scan_buffer_bytes_, buffer_bytes);
    row_group_ = group;
    if (group_rows_ > 0) return true;
  }
  return false;
}

arrow::Result<std::optional<Batch>> ScanOperator::Next() {
  for (;;) {
    // A selective scan can read the whole input inside this one call, so the check is here, once
    // per batch read and per row group, and not only where the caller gets a batch.
    ARROW_RETURN_NOT_OK(CheckInterrupted(*context_));
    if (position_ >= group_rows_) {
      ARROW_ASSIGN_OR_RAISE(const bool more, LoadNextRowGroup());
      if (!more) return std::optional<Batch>();
    }
    const int64_t length = std::min<int64_t>(context_->batch_size, group_rows_ - position_);
    Batch batch;
    batch.length = length;
    for (const auto& column : columns_) {
      batch.columns.push_back(column->Slice(position_, length));
    }
    position_ += length;
    if (node_.predicate != nullptr) {
      ARROW_ASSIGN_OR_RAISE(SelectionVector selection,
                            EvaluatePredicate(*node_.predicate, batch, context_->pool));
      if (selection.size() == 0) continue;
      if (selection.size() < length) batch.selection = std::move(selection);
    }
    return std::optional<Batch>(std::move(batch));
  }
}

void ScanOperator::Close() {
  files_.clear();
  columns_.clear();
}

void ScanOperator::FillStats(OperatorStats* stats) const {
  stats->scan_buffer_bytes = scan_buffer_bytes_;
}

}  // namespace quarry
