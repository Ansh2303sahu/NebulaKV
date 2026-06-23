include_guard(GLOBAL)

function(set_project_warnings target_name)
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /W4 /permissive-)
    if(WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE /WX)
    endif()
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      ${target_name}
      PRIVATE -Wall
              -Wextra
              -Wpedantic
              -Wconversion
              -Wsign-conversion
              -Wshadow
              -Wnon-virtual-dtor
              -Wold-style-cast
              -Wcast-align
              -Woverloaded-virtual
              -Wnull-dereference
              -Wdouble-promotion
              -Wformat=2)
    if(WARNINGS_AS_ERRORS)
      target_compile_options(${target_name} PRIVATE -Werror)
    endif()
  endif()
endfunction()

function(enable_project_sanitizers target_name)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_TSAN)
      message(WARNING "Sanitizers are configured only for GCC and Clang")
    endif()
    return()
  endif()

  set(sanitizers "")
  if(ENABLE_ASAN)
    list(APPEND sanitizers address)
  endif()
  if(ENABLE_UBSAN)
    list(APPEND sanitizers undefined)
  endif()
  if(ENABLE_TSAN)
    list(APPEND sanitizers thread)
  endif()

  if(sanitizers)
    list(JOIN sanitizers "," sanitizer_list)
    target_compile_options(
      ${target_name} PRIVATE -fsanitize=${sanitizer_list} -fno-omit-frame-pointer)
    target_link_options(
      ${target_name} PRIVATE -fsanitize=${sanitizer_list} -fno-omit-frame-pointer)
  endif()
endfunction()

function(enable_project_clang_tidy target_name)
  if(NOT ENABLE_CLANG_TIDY)
    return()
  endif()

  find_program(
    CLANG_TIDY_EXECUTABLE
    NAMES clang-tidy clang-tidy-20 clang-tidy-19 clang-tidy-18 clang-tidy-17
    REQUIRED)

  set_target_properties(
    ${target_name}
    PROPERTIES CXX_CLANG_TIDY
               "${CLANG_TIDY_EXECUTABLE};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy")
endfunction()

function(configure_project_target target_name)
  set_project_warnings(${target_name})
  enable_project_sanitizers(${target_name})
  enable_project_clang_tidy(${target_name})
endfunction()
