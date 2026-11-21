// libFuzzer target: arbitrary bytes as SQL text. Parse must neither crash nor recurse without bound
// (a stack overflow shows up as an ASan failure), and its answer must be consistent: a statement
// or an error that begins with the position prefix, never both and never neither.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "parser/parser.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const quarry::ParseResult result =
      quarry::Parse(std::string_view(reinterpret_cast<const char*>(data), size));
  if (result.ok() == !result.error.empty()) __builtin_trap();
  if (!result.ok() && result.error.rfind("syntax error at ", 0) != 0) __builtin_trap();
  return 0;
}
