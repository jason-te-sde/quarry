#include "output_format.h"

#include <arrow/array.h>
#include <arrow/array/concatenate.h>
#include <arrow/array/util.h>
#include <arrow/chunked_array.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "types/date.h"
#include "types/decimal.h"
#include "types/logical_type.h"

namespace quarry {
namespace {

enum class Style : uint8_t { kJson, kPlain };

// One array per column; a column with no chunk is an empty array.
std::vector<std::shared_ptr<arrow::Array>> FlatColumns(const arrow::Table& table) {
  std::vector<std::shared_ptr<arrow::Array>> columns;
  for (const auto& column : table.columns()) {
    if (column->num_chunks() == 1) {
      columns.push_back(column->chunk(0));
    } else if (column->num_chunks() == 0) {
      columns.push_back(arrow::MakeEmptyArray(column->type()).ValueOrDie());
    } else {
      columns.push_back(arrow::Concatenate(column->chunks()).ValueOrDie());
    }
  }
  return columns;
}

std::string TypeName(const arrow::DataType& type) {
  auto logical = LogicalType::FromArrow(type);
  return logical.ok() ? logical->ToString() : type.ToString();
}

void AppendJsonString(std::string_view text, std::string* out) {
  out->push_back('"');
  for (const char ch : text) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        *out += "\\\"";
        break;
      case '\\':
        *out += "\\\\";
        break;
      case '\n':
        *out += "\\n";
        break;
      case '\r':
        *out += "\\r";
        break;
      case '\t':
        *out += "\\t";
        break;
      case '\b':
        *out += "\\b";
        break;
      case '\f':
        *out += "\\f";
        break;
      default:
        if (c < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          *out += buffer;
        } else {
          out->push_back(ch);
        }
    }
  }
  out->push_back('"');
}

// The value of row i as text. For JSON a string is quoted and a NULL is "null"; the plain style
// writes the NULL as an empty string for the caller to replace.
std::string Value(const arrow::Array& array, int64_t i, Style style) {
  if (array.IsNull(i)) return style == Style::kJson ? "null" : "";
  std::string out;
  switch (array.type_id()) {
    case arrow::Type::BOOL:
      return static_cast<const arrow::BooleanArray&>(array).Value(i) ? "true" : "false";
    case arrow::Type::INT32:
      return std::to_string(static_cast<const arrow::Int32Array&>(array).Value(i));
    case arrow::Type::INT64:
      return std::to_string(static_cast<const arrow::Int64Array&>(array).Value(i));
    case arrow::Type::DOUBLE: {
      const double v = static_cast<const arrow::DoubleArray&>(array).Value(i);
      if (!std::isfinite(v))
        return style == Style::kJson ? "null"
                                     : (std::isnan(v) ? "NaN" : (v < 0 ? "-Infinity" : "Infinity"));
      char buffer[40];
      if (style == Style::kJson) {
        std::snprintf(buffer, sizeof(buffer), "%.17g", v);
        return buffer;
      }
      const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), v);
      (void)ec;
      return std::string(buffer, end);
    }
    case arrow::Type::DATE32: {
      std::string date = FormatDate(static_cast<const arrow::Date32Array&>(array).Value(i));
      if (style == Style::kJson) {
        AppendJsonString(date, &out);
        return out;
      }
      return date;
    }
    case arrow::Type::DECIMAL64: {
      int64_t value;
      std::memcpy(&value, static_cast<const arrow::Decimal64Array&>(array).GetValue(i), 8);
      std::string text =
          DecimalToString(value, static_cast<const arrow::Decimal64Type&>(*array.type()).scale());
      if (style == Style::kJson) return "\"" + text + "\"";
      return text;
    }
    case arrow::Type::DECIMAL128: {
      Int128 value;
      std::memcpy(&value, static_cast<const arrow::Decimal128Array&>(array).GetValue(i), 16);
      std::string text =
          DecimalToString(value, static_cast<const arrow::Decimal128Type&>(*array.type()).scale());
      if (style == Style::kJson) return "\"" + text + "\"";
      return text;
    }
    case arrow::Type::STRING: {
      const std::string_view text = static_cast<const arrow::StringArray&>(array).GetView(i);
      if (style == Style::kJson) {
        AppendJsonString(text, &out);
        return out;
      }
      return std::string(text);
    }
    default:
      return "?";
  }
}

std::string CsvField(const std::string& text) {
  if (text.find_first_of(",\"\r\n") == std::string::npos) return text;
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

bool IsNumeric(const arrow::DataType& type) {
  switch (type.id()) {
    case arrow::Type::INT32:
    case arrow::Type::INT64:
    case arrow::Type::DOUBLE:
    case arrow::Type::DECIMAL64:
    case arrow::Type::DECIMAL128:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::string ToJson(const arrow::Table& table) {
  const auto columns = FlatColumns(table);
  std::string out = "{\"columns\":[";
  for (int c = 0; c < table.num_columns(); ++c) {
    if (c > 0) out += ',';
    out += "{\"name\":";
    AppendJsonString(table.schema()->field(c)->name(), &out);
    out += ",\"type\":";
    AppendJsonString(TypeName(*table.schema()->field(c)->type()), &out);
    out += '}';
  }
  out += "],\"rows\":[";
  for (int64_t r = 0; r < table.num_rows(); ++r) {
    if (r > 0) out += ',';
    out += '[';
    for (size_t c = 0; c < columns.size(); ++c) {
      if (c > 0) out += ',';
      out += Value(*columns[c], r, Style::kJson);
    }
    out += ']';
  }
  out += "]}";
  return out;
}

std::string ToCsv(const arrow::Table& table) {
  const auto columns = FlatColumns(table);
  std::string out;
  for (int c = 0; c < table.num_columns(); ++c) {
    if (c > 0) out += ',';
    out += CsvField(table.schema()->field(c)->name());
  }
  out += '\n';
  for (int64_t r = 0; r < table.num_rows(); ++r) {
    for (size_t c = 0; c < columns.size(); ++c) {
      if (c > 0) out += ',';
      out += CsvField(Value(*columns[c], r, Style::kPlain));
    }
    out += '\n';
  }
  return out;
}

std::string ToTable(const arrow::Table& table) {
  const auto columns = FlatColumns(table);
  const auto width_count = columns.size();
  std::vector<std::vector<std::string>> cells(static_cast<size_t>(table.num_rows()),
                                              std::vector<std::string>(width_count));
  std::vector<size_t> widths(width_count);
  for (size_t c = 0; c < width_count; ++c) {
    widths[c] = table.schema()->field(static_cast<int>(c))->name().size();
  }
  for (int64_t r = 0; r < table.num_rows(); ++r) {
    for (size_t c = 0; c < width_count; ++c) {
      std::string text = columns[c]->IsNull(r) ? "NULL" : Value(*columns[c], r, Style::kPlain);
      widths[c] = std::max(widths[c], text.size());
      cells[static_cast<size_t>(r)][c] = std::move(text);
    }
  }
  const auto pad = [](const std::string& text, size_t width, bool right) {
    const std::string fill(width - std::min(width, text.size()), ' ');
    return right ? fill + text : text + fill;
  };
  std::string out;
  for (size_t c = 0; c < width_count; ++c) {
    out += (c == 0 ? "" : " | ") + pad(table.schema()->field(static_cast<int>(c))->name(),
                                       widths[c], IsNumeric(*columns[c]->type()));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  out += '\n';
  for (size_t c = 0; c < width_count; ++c) {
    out += (c == 0 ? "" : "-+-") + std::string(widths[c], '-');
  }
  out += '\n';
  for (const auto& row : cells) {
    std::string line;
    for (size_t c = 0; c < width_count; ++c) {
      line += (c == 0 ? "" : " | ") + pad(row[c], widths[c], IsNumeric(*columns[c]->type()));
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    out += line + '\n';
  }
  out += "(" + std::to_string(table.num_rows()) + (table.num_rows() == 1 ? " row)\n" : " rows)\n");
  return out;
}

}  // namespace quarry
