include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(project_benchmarks core_benchmark.cpp)
target_link_libraries(
  project_benchmarks PRIVATE industry_starter::core benchmark::benchmark_main)
configure_project_target(project_benchmarks)
