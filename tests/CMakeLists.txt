include(GoogleTest)

function(mr_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE minirecorder mr_warnings GTest::gtest GTest::gtest_main)
  gtest_discover_tests(${name} DISCOVERY_TIMEOUT 30 PROPERTIES LABELS unit)
endfunction()

mr_add_test(test_crc32c test_crc32c.cpp)
mr_add_test(test_spsc_queue test_spsc_queue.cpp)
mr_add_test(test_record_ring test_record_ring.cpp)
mr_add_test(test_format test_format.cpp)
