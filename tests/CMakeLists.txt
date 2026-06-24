include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(nebulakv_crash_writer support/crash_writer.cpp)
target_link_libraries(nebulakv_crash_writer PRIVATE NebulaKV::core)
configure_project_target(nebulakv_crash_writer)

add_executable(
  nebulakv_tests
  block_cache_test.cpp
  bloom_filter_test.cpp
  bounded_executor_test.cpp
  checksum_calculator_test.cpp
  in_memory_key_value_store_test.cpp
  memtable_test.cpp
  memtable_set_test.cpp
  persistent_key_value_store_test.cpp
  recovery_manager_test.cpp
  request_processor_test.cpp
  sstable_manager_test.cpp
  sstable_writer_reader_test.cpp
  wal_writer_reader_test.cpp)
target_link_libraries(
  nebulakv_tests
  PRIVATE NebulaKV::core NebulaKV::service_runtime GTest::gtest_main)

if(ENABLE_GRPC AND ENABLE_GRPC_INTEGRATION_TESTS)
  target_sources(nebulakv_tests PRIVATE grpc_service_test.cpp)
  target_link_libraries(nebulakv_tests PRIVATE NebulaKV::network)
endif()
target_compile_definitions(
  nebulakv_tests
  PRIVATE NEBULAKV_CRASH_WRITER_PATH="$<TARGET_FILE:nebulakv_crash_writer>")
add_dependencies(nebulakv_tests nebulakv_crash_writer)
configure_project_target(nebulakv_tests)

include(GoogleTest)
gtest_discover_tests(nebulakv_tests DISCOVERY_TIMEOUT 30)
