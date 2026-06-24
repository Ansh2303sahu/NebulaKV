include(FetchContent)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  googlebenchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(googlebenchmark)

add_executable(
  nebulakv_benchmarks
  compaction_benchmark.cpp
  key_value_store_benchmark.cpp
  raft_benchmark.cpp
  sstable_benchmark.cpp)
target_link_libraries(
  nebulakv_benchmarks
  PRIVATE NebulaKV::core NebulaKV::raft benchmark::benchmark_main)
configure_project_target(nebulakv_benchmarks)
