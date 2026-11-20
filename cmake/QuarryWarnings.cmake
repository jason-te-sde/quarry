# The warning set for every quarry target. It is applied per target, by quarry_set_warnings() in
# QuarryTargets.cmake, and deliberately not with a directory-wide add_compile_options(): that would
# also reach the GoogleTest and Google Benchmark sources fetched later, and -Werror must never
# apply to them.

set(QUARRY_WARNING_FLAGS
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror)
