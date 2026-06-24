include_guard(GLOBAL)

file(
  GLOB_RECURSE PROJECT_FORMAT_FILES
  CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/app/*.cpp"
  "${PROJECT_SOURCE_DIR}/benchmarks/*.cpp"
  "${PROJECT_SOURCE_DIR}/client/*.cpp"
  "${PROJECT_SOURCE_DIR}/include/*.h"
  "${PROJECT_SOURCE_DIR}/include/*.hpp"
  "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/*.hpp"
  "${PROJECT_SOURCE_DIR}/server/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.hpp")

find_program(
  CLANG_FORMAT_EXECUTABLE
  NAMES clang-format clang-format-20 clang-format-19 clang-format-18 clang-format-17)

if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(
    format
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${PROJECT_FORMAT_FILES}
    COMMENT "Formatting C++ source files with clang-format"
    VERBATIM)

  add_custom_target(
    format-check
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" --dry-run --Werror ${PROJECT_FORMAT_FILES}
    COMMENT "Checking C++ source formatting"
    VERBATIM)
else()
  add_custom_target(
    format
    COMMAND "${CMAKE_COMMAND}" -E echo
            "clang-format was not found. Install clang-format and reconfigure."
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM)
  add_custom_target(
    format-check
    COMMAND "${CMAKE_COMMAND}" -E echo
            "clang-format was not found. Install clang-format and reconfigure."
    COMMAND "${CMAKE_COMMAND}" -E false
    VERBATIM)
endif()
