if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "MR_BUILD_FUZZERS requires clang (libFuzzer)")
endif()

add_executable(fuzz_chunk fuzz_chunk.cpp)
target_compile_options(fuzz_chunk PRIVATE -fsanitize=fuzzer)
target_link_options(fuzz_chunk PRIVATE -fsanitize=fuzzer)
target_link_libraries(fuzz_chunk PRIVATE minirecorder mr_warnings)

add_executable(fuzz_make_seeds make_seeds.cpp)
target_link_libraries(fuzz_make_seeds PRIVATE minirecorder mr_warnings)
