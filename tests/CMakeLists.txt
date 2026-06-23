include(FetchContent)

set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(project_tests core_test.cpp)
target_link_libraries(project_tests PRIVATE industry_starter::core GTest::gtest_main)
configure_project_target(project_tests)

include(GoogleTest)
gtest_discover_tests(project_tests DISCOVERY_TIMEOUT 30)
