cmake_minimum_required(VERSION 3.22)

project(MiniRecorder
  VERSION 0.1.0
  DESCRIPTION "Multi-threaded sensor data recorder with a crash-safe log storage engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MR_BUILD_TESTS "Build unit tests" ON)
option(MR_BUILD_BENCHMARKS "Build Google Benchmark micro-benchmarks" ON)
option(MR_BUILD_FUZZERS "Build libFuzzer targets (clang only)" OFF)
option(MR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
set(MR_SANITIZE "" CACHE STRING "Sanitizers to enable: address,undefined | thread | empty")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

if(MR_BUILD_FUZZERS)
  # Coverage instrumentation for all code the fuzzer reaches; only the fuzz
  # target itself links the libFuzzer main.
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()

add_subdirectory(src)
add_subdirectory(tools)

if(MR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MR_BUILD_BENCHMARKS)
  add_subdirectory(bench)
endif()

if(MR_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
