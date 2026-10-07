function(mr_add_bench name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE minirecorder mr_warnings benchmark::benchmark)
endfunction()

mr_add_bench(bench_ring)
mr_add_bench(bench_codec)
