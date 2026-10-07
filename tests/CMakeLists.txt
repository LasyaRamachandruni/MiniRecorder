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
mr_add_test(test_writer test_writer.cpp)
mr_add_test(test_reader test_reader.cpp)
mr_add_test(test_recovery test_recovery.cpp)
mr_add_test(test_sensors test_sensors.cpp)
mr_add_test(test_quality test_quality.cpp)
mr_add_test(test_trigger test_trigger.cpp)
mr_add_test(test_privacy test_privacy.cpp)
mr_add_test(test_recorder test_recorder.cpp)
mr_add_test(test_kitti test_kitti.cpp)

# End-to-end: spawns the real mr_record binary and SIGKILLs it.
mr_add_test(test_crash_kill test_crash_kill.cpp)
target_compile_definitions(test_crash_kill PRIVATE MR_RECORD_PATH="$<TARGET_FILE:mr_record>")
add_dependencies(test_crash_kill mr_record)

add_test(NAME e2e_smoke
  COMMAND ${PROJECT_SOURCE_DIR}/scripts/e2e_smoke.sh $<TARGET_FILE_DIR:mr_record>)
set_tests_properties(e2e_smoke PROPERTIES LABELS e2e TIMEOUT 120)
