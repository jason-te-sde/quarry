#include "binder/catalog.h"

#include <algorithm>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace quarry {
namespace {

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

bool IsParquetFile(const std::filesystem::path& path) {
  return Lower(path.extension().string()) == ".parquet";
}

// The index of the field of `schema` whose lower-cased name is `lower_name`, or -1.
int FindField(const arrow::Schema& schema, const std::string& lower_name) {
  for (int i = 0; i < schema.num_fields(); ++i) {
    if (Lower(schema.field(i)->name()) == lower_name) return i;
  }
  return -1;
}

}  // namespace

arrow::Status Catalog::AddTable(std::string name, std::vector<std::filesystem::path> files) {
  name = Lower(std::move(name));
  if (name.empty()) return arrow::Status::Invalid("a table needs a name");
  if (files.empty()) return arrow::Status::Invalid("table ", name, " has no Parquet file");
  if (tables_.find(name) != tables_.end()) {
    return arrow::Status::Invalid("table ", name, " is already registered");
  }

  TableInfo table;
  table.name = name;
  for (const std::filesystem::path& file : files) {
    ARROW_ASSIGN_OR_RAISE(ParquetFileInfo info, ReadParquetMetadata(file));
    table.row_count += info.num_rows;
    table.file_info.push_back(std::move(info));
  }
  table.files = std::move(files);

  const arrow::Schema& first = *table.file_info[0].schema;
  std::unordered_set<std::string> seen;
  for (const auto& field : first.fields()) {
    ColumnInfo column;
    column.name = Lower(field->name());
    if (!seen.insert(column.name).second) {
      return arrow::Status::Invalid("table ", name, " has two columns named ", column.name,
                                    " once lower-cased");
    }
    const std::string where = "column " + column.name + " of table " + name;
    for (size_t f = 0; f < table.files.size() && column.unsupported_reason.empty(); ++f) {
      const std::string file = table.files[f].string();
      const arrow::Schema& schema = *table.file_info[f].schema;
      const int index = FindField(schema, column.name);
      if (index < 0) {
        column.unsupported_reason = where;
        column.unsupported_reason += " is missing from ";
        column.unsupported_reason += file;
        break;
      }
      auto type = LogicalType::FromArrow(*schema.field(index)->type());
      if (!type.ok()) {
        column.unsupported_reason = where;
        column.unsupported_reason += " has an unsupported type in ";
        column.unsupported_reason += file;
        column.unsupported_reason += ": ";
        column.unsupported_reason += type.status().message();
        break;
      }
      if (!column.type.has_value()) {
        column.type = *type;
      } else if (!(*column.type == *type)) {
        column.unsupported_reason = where;
        column.unsupported_reason += " has type ";
        column.unsupported_reason += column.type->ToString();
        column.unsupported_reason += " in ";
        column.unsupported_reason += table.files[0].string();
        column.unsupported_reason += " but ";
        column.unsupported_reason += type->ToString();
        column.unsupported_reason += " in ";
        column.unsupported_reason += file;
      }
    }
    if (!column.unsupported_reason.empty()) column.type.reset();
    table.columns.push_back(std::move(column));
  }
  tables_.emplace(name, std::move(table));
  return arrow::Status::OK();
}

arrow::Status Catalog::AddDirectory(const std::filesystem::path& directory) {
  std::error_code ec;
  if (!std::filesystem::is_directory(directory, ec)) {
    return arrow::Status::Invalid(directory.string(), " is not a directory");
  }
  std::vector<std::filesystem::path> entries;
  for (std::filesystem::directory_iterator it(directory, ec), end; !ec && it != end;
       it.increment(ec)) {
    entries.push_back(it->path());
  }
  if (ec) return arrow::Status::IOError("cannot list ", directory.string(), ": ", ec.message());
  std::sort(entries.begin(), entries.end());

  for (const std::filesystem::path& entry : entries) {
    if (std::filesystem::is_directory(entry, ec)) {
      std::vector<std::filesystem::path> files;
      std::vector<std::filesystem::path> children;
      for (std::filesystem::directory_iterator it(entry, ec), end; !ec && it != end;
           it.increment(ec)) {
        children.push_back(it->path());
      }
      if (ec) return arrow::Status::IOError("cannot list ", entry.string(), ": ", ec.message());
      std::sort(children.begin(), children.end());
      for (const std::filesystem::path& child : children) {
        if (IsParquetFile(child) && std::filesystem::is_regular_file(child, ec)) {
          files.push_back(child);
        }
      }
      if (!files.empty()) {
        ARROW_RETURN_NOT_OK(AddTable(entry.filename().string(), std::move(files)));
      }
    } else if (IsParquetFile(entry) && std::filesystem::is_regular_file(entry, ec)) {
      ARROW_RETURN_NOT_OK(AddTable(entry.stem().string(), {entry}));
    }
  }
  return arrow::Status::OK();
}

const TableInfo* Catalog::Find(std::string_view name) const {
  const auto it = tables_.find(name);
  return it == tables_.end() ? nullptr : &it->second;
}

}  // namespace quarry
