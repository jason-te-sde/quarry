#ifndef QUARRY_TOOLS_OUTPUT_FORMAT_H_
#define QUARRY_TOOLS_OUTPUT_FORMAT_H_

#include <arrow/table.h>

#include <string>

namespace quarry {

// The JSON result format shared with scripts/gen_tpch.py, byte for byte:
//   {"columns":[{"name":"x","type":"DECIMAL(38,2)"}],"rows":[["1.50"],[null]]}
// No whitespace. A DECIMAL is a string with exactly `scale` decimals, a DOUBLE a number printed
// with %.17g, a DATE "YYYY-MM-DD", a NULL null, a BOOLEAN true or false, a BIGINT or INTEGER a
// number, a VARCHAR a string escaped as Python's json.dumps(ensure_ascii=False) does. A NaN or
// infinite DOUBLE, which JSON cannot hold, is null.
std::string ToJson(const arrow::Table& table);

// A header line and one line per row. NULL is empty; a field with a comma, quote or line break is
// quoted.
std::string ToCsv(const arrow::Table& table);

// An aligned text table with the header, a rule and "(N rows)" at the end; NULL is "NULL".
std::string ToTable(const arrow::Table& table);

}  // namespace quarry

#endif  // QUARRY_TOOLS_OUTPUT_FORMAT_H_
