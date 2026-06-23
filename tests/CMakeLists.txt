include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(nebulakv_tests in_memory_key_value_store_test.cpp)
target_link_libraries(nebulakv_tests PRIVATE NebulaKV::core GTest::gtest_main)
configure_project_target(nebulakv_tests)

include(GoogleTest)
gtest_discover_tests(nebulakv_tests DISCOVERY_TIMEOUT 30)
