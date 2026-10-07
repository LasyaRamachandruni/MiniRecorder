# Project-wide warning flags, applied through an interface target so that
# third-party code pulled in via FetchContent is not affected.

add_library(mr_warnings INTERFACE)

target_compile_options(mr_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Wunused
  -Woverloaded-virtual
  -Wconversion
  -Wsign-conversion
  -Wnull-dereference
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_options(mr_warnings INTERFACE
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast
  )
endif()

if(MR_WARNINGS_AS_ERRORS)
  target_compile_options(mr_warnings INTERFACE -Werror)
endif()
