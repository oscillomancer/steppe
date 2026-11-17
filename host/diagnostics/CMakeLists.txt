# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
add_custom_target(diagnostics-build-id
  COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR} -P "${CMAKE_CURRENT_SOURCE_DIR}/build_id.cmake"
  BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/diagnostics_build.h"
  VERBATIM)
if(WIN32)
  set(native_source src/native_windows.cpp)
else()
  set(native_source src/native_posix.cpp src/capsule_posix.cpp)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(trace_source src/trace_linux.cpp)
elseif(WIN32)
  set(trace_source src/trace_windows.cpp)
elseif(APPLE)
  set(trace_source src/trace_other.cpp src/capture_macos.cpp)
else()
  set(trace_source src/trace_other.cpp)
endif()
supple_add_library(diagnostics SOURCES src/frontend.cpp src/terminal.cpp src/storage.cpp src/private_files.cpp src/trace.cpp ${native_source} ${trace_source}
  DEPS supple::core-base supple::core-json supple::product-info)
add_dependencies(diagnostics diagnostics-build-id)
target_include_directories(diagnostics PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(WIN32)
  target_link_libraries(diagnostics PRIVATE advapi32 bcrypt dbghelp psapi shell32)
endif()
if(SUPPLE_BUILD_TESTS)
  add_executable(diagnostics-fixture tests/fixture.cpp)
  target_link_libraries(diagnostics-fixture PRIVATE supple::diagnostics)
  supple_target_defaults(diagnostics-fixture)
  supple_add_tests(diagnostics SOURCES tests/test_storage.cpp tests/test_exit.cpp tests/test_trace.cpp tests/test_capsule.cpp)
  target_compile_definitions(diagnostics.tests PRIVATE DIAGNOSTICS_FIXTURE="$<TARGET_FILE:diagnostics-fixture>")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME diagnostics.lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle.py" "$<TARGET_FILE:diagnostics-fixture>" "${SUPPLE_SANITIZE}")
  set_tests_properties(diagnostics.lifecycle PROPERTIES LABELS rt)
endif()
