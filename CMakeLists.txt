cmake_minimum_required(VERSION 3.22)

project(
  industry_starter
  VERSION 0.1.0
  DESCRIPTION "Production-style C++20 project foundation"
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

add_library(project_core src/core.cpp)
add_library(industry_starter::core ALIAS project_core)

target_include_directories(
  project_core
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>)
target_compile_features(project_core PUBLIC cxx_std_20)
configure_project_target(project_core)

add_executable(project_cli app/main.cpp)
target_link_libraries(project_cli PRIVATE industry_starter::core)
configure_project_target(project_cli)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
