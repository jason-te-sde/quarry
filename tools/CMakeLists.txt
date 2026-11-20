add_executable(quarry quarry_main.cc)
target_link_libraries(quarry PRIVATE quarry_engine quarry_io)
quarry_set_warnings(quarry)
