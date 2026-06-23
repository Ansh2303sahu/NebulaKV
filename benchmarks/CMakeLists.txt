include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(nebulakv_benchmarks key_value_store_benchmark.cpp)
target_link_libraries(nebulakv_benchmarks PRIVATE NebulaKV::core benchmark::benchmark_main)
configure_project_target(nebulakv_benchmarks)
