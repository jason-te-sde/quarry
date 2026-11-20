# libFuzzer targets (Clang only; QUARRY_FUZZ in the top-level CMakeLists.txt). Each is built with
# AddressSanitizer and UBSan, which the QUARRY_FUZZ option insists on.
#
# Run one: build/fuzz/fuzz/fuzz_parser <corpus-dir> -max_total_time=60

function(quarry_add_fuzz_target name)
  add_executable(${name} ${name}.cc)
  target_include_directories(${name} PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(${name} PRIVATE ${ARGN})
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
  set_target_properties(${name} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/fuzz")
  quarry_set_warnings(${name})
endfunction()

quarry_add_fuzz_target(fuzz_parser quarry_parser)
