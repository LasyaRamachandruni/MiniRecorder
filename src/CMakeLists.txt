add_library(minirecorder
  chunk.cpp
  file_util.cpp
  crc32c.cpp
  events.cpp
  format.cpp
  record.cpp
  reader.cpp
  record_ring.cpp
  recovery.cpp
  writer.cpp
  result.cpp
)

target_include_directories(minirecorder PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(minirecorder
  PUBLIC Threads::Threads
  PRIVATE mr_warnings ${MR_ZSTD_TARGET})
