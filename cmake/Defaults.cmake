# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Shared target helpers. Every first-party target goes through
# supple_target_defaults(); third-party targets never do.

set(SUPPLE_SANITIZE "" CACHE STRING
    "Sanitizer set for first-party targets: '', address, undefined, thread, realtime (comma-separated)")
option(SUPPLE_WERROR "Treat warnings as errors on first-party targets" OFF)

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-Wno-perf-constraint-implies-noexcept SUPPLE_APPLE_RT_NOEXCEPT_WARNING)
endif()

set(_supple_sanitize_flags "")
if(SUPPLE_SANITIZE)
  string(REPLACE "," ";" _san_list "${SUPPLE_SANITIZE}")
  foreach(_san IN LISTS _san_list)
    if(_san STREQUAL "address")
      list(APPEND _supple_sanitize_flags -fsanitize=address -fno-omit-frame-pointer)
    elseif(_san STREQUAL "undefined")
      list(APPEND _supple_sanitize_flags -fsanitize=undefined -fno-sanitize-recover=undefined)
    elseif(_san STREQUAL "thread")
      list(APPEND _supple_sanitize_flags -fsanitize=thread)
    elseif(_san STREQUAL "realtime")
      if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "RealtimeSanitizer requires Clang")
      endif()
      list(APPEND _supple_sanitize_flags -fsanitize=realtime)
    else()
      message(FATAL_ERROR "Unknown sanitizer '${_san}'")
    endif()
  endforeach()
endif()

# clang-cl accepts warning switches directly, but ignores some GNU driver
# flags (notably -ffp-contract). Forward every such option explicitly.
function(supple_compile_options target scope)
  set(flags ${ARGN})
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    list(TRANSFORM flags REPLACE "^(-.*)$" "/clang:\\1")
    list(TRANSFORM flags REPLACE ":(-W[^>]+)>$" ":/clang:\\1>")
  endif()
  target_compile_options(${target} ${scope} ${flags})
endfunction()

function(supple_target_defaults target)
  target_compile_features(${target} PUBLIC cxx_std_20)
  set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)
  get_target_property(_type ${target} TYPE)
  if(_type STREQUAL "INTERFACE_LIBRARY")
    return()
  endif()
  # Determinism (Principle 5): no FMA contraction, so Clang and GCC compute
  # identical DSP and analysis results. Never add -ffast-math.
  set(flags -ffp-contract=off)
  list(APPEND flags -Wall -Wextra -Wpedantic -Wshadow
                                           -Wcast-align -Wnull-dereference -Wdouble-promotion
                                           $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
                                           $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
                                           $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>)
  supple_compile_options(${target} PRIVATE ${flags})
  # Parameter tables use designated initializers with default member values
  # on purpose (docs/specs/core-params.md); Clang's -Wextra would flag every
  # omitted field.
  supple_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-missing-designated-field-initializers>
    $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>)
  if(SUPPLE_WERROR)
    target_compile_options(${target} PRIVATE $<IF:$<BOOL:${MSVC}>,/WX,-Werror>)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND SUPPLE_APPLE_RT_NOEXCEPT_WARNING)
    # AppleClang diagnoses existing nonblocking signatures without explicit
    # noexcept. Keep their types and RT annotations; suppress only this
    # signature advisory, not function-effect checks or other warnings.
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wno-perf-constraint-implies-noexcept>)
  endif()
  if(_supple_sanitize_flags)
    target_compile_options(${target} PRIVATE ${_supple_sanitize_flags})
    target_link_options(${target} PRIVATE ${_supple_sanitize_flags})
  endif()
endfunction()

# supple_add_library(<name> [SOURCES ...] [DEPS ...])
# Creates target <name> (e.g. core-json) with alias supple::<name>.
# Header-only when SOURCES is empty.
function(supple_add_library name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  if(ARG_SOURCES)
    add_library(${name} STATIC ${ARG_SOURCES})
    target_include_directories(${name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
                                       PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(${name} PUBLIC ${ARG_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
  else()
    add_library(${name} INTERFACE)
    target_include_directories(${name} INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(${name} INTERFACE ${ARG_DEPS})
  endif()
  supple_target_defaults(${name})
  add_library(supple::${name} ALIAS ${name})
endfunction()

# supple_add_tests(<lib> SOURCES ... [DEPS ...] [LABELS ...] [SPLIT])
# Builds <lib>.tests (doctest) and registers it with CTest. SPLIT registers
# one CTest entry per source file instead (<lib>.tests.<file stem>, doctest's
# source-file filter), so a large suite runs in parallel and each part stays
# within the test timeout (under the sanitizers, and on slower CI runners).
function(supple_add_tests lib)
  if(NOT SUPPLE_BUILD_TESTS)
    return()
  endif()
  cmake_parse_arguments(ARG "SPLIT" "" "SOURCES;DEPS;LABELS" ${ARGN})
  set(t ${lib}.tests)
  add_executable(${t} ${ARG_SOURCES})
  target_link_libraries(${t} PRIVATE ${lib} ${ARG_DEPS} supple::doctest-main)
  supple_target_defaults(${t})
  set(names)
  if(ARG_SPLIT)
    foreach(src ${ARG_SOURCES})
      get_filename_component(stem ${src} NAME_WE)
      # Either separator: clang-cl's __FILE__ uses backslashes.
      add_test(NAME ${t}.${stem} COMMAND ${t} "-sf=*/${stem}.cpp,*\\${stem}.cpp")
      list(APPEND names ${t}.${stem})
    endforeach()
  else()
    add_test(NAME ${t} COMMAND ${t})
    set(names ${t})
  endif()
  set_tests_properties(${names} PROPERTIES LABELS "${lib};${ARG_LABELS}")
  if(ARG_SPLIT)
    # A filter that matched nothing would pass silently: fail it instead.
    set_tests_properties(${names} PROPERTIES FAIL_REGULAR_EXPRESSION "test cases: +0 [|]")
  endif()
  if(ARG_LABELS MATCHES "rt")
    set_tests_properties(${names} PROPERTIES ENVIRONMENT
      "RTSAN_OPTIONS=halt_on_error=1;UBSAN_OPTIONS=print_stacktrace=1")
  endif()
endfunction()
