add_library(minirecorder
  chunk.cpp
  crc32c.cpp
  format.cpp
  record.cpp
  record_ring.cpp
  result.cpp
)

target_include_directories(minirecorder PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minirecorder
  PUBLIC Threads::Threads
  PRIVATE mr_warnings ${MR_ZSTD_TARGET})
