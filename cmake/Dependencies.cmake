# Dependency resolution: prefer system packages (fast, reproducible in the
# Dockerfile/CI), fall back to FetchContent for GoogleTest and Google
# Benchmark when they are not installed.

include(FetchContent)

find_package(PkgConfig QUIET)
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_shared)
  set(MR_ZSTD_TARGET zstd::libzstd_shared)
elseif(TARGET zstd::libzstd_static)
  set(MR_ZSTD_TARGET zstd::libzstd_static)
elseif(PkgConfig_FOUND)
  pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
  set(MR_ZSTD_TARGET PkgConfig::ZSTD)
else()
  message(FATAL_ERROR "libzstd not found: install libzstd-dev")
endif()

find_package(Threads REQUIRED)

if(MR_BUILD_TESTS)
  find_package(GTest CONFIG QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found, fetching v1.14.0")
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()

if(MR_BUILD_BENCHMARKS)
  find_package(benchmark CONFIG QUIET)
  if(NOT benchmark_FOUND)
    message(STATUS "Google Benchmark not found, fetching v1.8.3")
    FetchContent_Declare(benchmark
      URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.tar.gz)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
  endif()
endif()
