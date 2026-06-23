cmake_minimum_required(VERSION 3.22)

project(
  NebulaKV
  VERSION 0.6.0
  DESCRIPTION "Durable distributed key-value database built in modern C++"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
option(ENABLE_BENCHMARKS "Build Google Benchmark targets" OFF)
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be enabled together")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(Formatting)

find_package(Threads REQUIRED)

add_library(
  nebulakv_core
  src/block_cache.cpp
  src/bloom_filter.cpp
  src/checksum_calculator.cpp
  src/in_memory_key_value_store.cpp
  src/memtable.cpp
  src/memtable_set.cpp
  src/persistent_key_value_store.cpp
  src/recovery_manager.cpp
  src/sstable_format.cpp
  src/sstable_manager.cpp
  src/sstable_reader.cpp
  src/sstable_writer.cpp
  src/validation.cpp
  src/wal_format.cpp
  src/wal_reader.cpp
  src/wal_writer.cpp)
add_library(NebulaKV::core ALIAS nebulakv_core)

target_include_directories(
  nebulakv_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_compile_features(nebulakv_core PUBLIC cxx_std_20)
target_link_libraries(nebulakv_core PUBLIC Threads::Threads)
configure_project_target(nebulakv_core)

add_executable(nebulakv_cli app/main.cpp)
target_link_libraries(nebulakv_cli PRIVATE NebulaKV::core)
configure_project_target(nebulakv_cli)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
