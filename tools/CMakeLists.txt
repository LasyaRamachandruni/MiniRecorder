add_library(mr_cli STATIC cli.cpp)
target_include_directories(mr_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mr_cli PUBLIC minirecorder PRIVATE mr_warnings)

function(mr_add_tool name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE mr_cli minirecorder mr_warnings)
endfunction()

mr_add_tool(mr_recover)
