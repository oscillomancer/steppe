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
supple_add_library(diagnostics SOURCES src/frontend.cpp src/inventory.cpp src/terminal.cpp src/storage.cpp src/watch.cpp src/private_files.cpp src/trace.cpp ${native_source} ${trace_source}
  DEPS supple::core-base supple::core-json supple::product-info supple::diagnostics-context)
add_dependencies(diagnostics diagnostics-build-id)
target_include_directories(diagnostics PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if(WIN32)
  target_link_libraries(diagnostics PRIVATE advapi32 bcrypt dbghelp psapi shell32)
endif()
add_subdirectory(resolver)
if(SUPPLE_BUILD_TESTS)
  add_executable(diagnostics-fixture tests/fixture.cpp)
  target_link_libraries(diagnostics-fixture PRIVATE supple::diagnostics supple::command-bus)
  supple_target_defaults(diagnostics-fixture)
  supple_add_tests(diagnostics SOURCES tests/test_storage.cpp tests/test_exit.cpp tests/test_trace.cpp tests/test_capsule.cpp tests/test_inventory.cpp "${PROJECT_SOURCE_DIR}/tests/progress_listener.cpp")
  target_compile_definitions(diagnostics.tests PRIVATE PROGRESS_SUITE="diagnostics.tests" DIAGNOSTICS_FIXTURE="$<TARGET_FILE:diagnostics-fixture>")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME diagnostics.lifecycle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle.py" "$<TARGET_FILE:diagnostics-fixture>" "${SUPPLE_SANITIZE}" "$<TARGET_FILE:resolve-crash>")
  set_tests_properties(diagnostics.lifecycle PROPERTIES LABELS rt)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Fault injection for the collector's ptrace calls (review round 29): the fixture runs
    # with this library preloaded. Skipped (77) in sanitized builds and under restricted tracing.
    add_library(diagnostics-ptrace-shim MODULE tests/ptrace_shim.cpp)
    target_link_libraries(diagnostics-ptrace-shim PRIVATE ${CMAKE_DL_LIBS})
    supple_target_defaults(diagnostics-ptrace-shim)
    add_test(NAME diagnostics.ptrace-faults COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ptrace_faults.py" "$<TARGET_FILE:diagnostics-fixture>" "$<TARGET_FILE:diagnostics-ptrace-shim>" "${SUPPLE_SANITIZE}")
    set_tests_properties(diagnostics.ptrace-faults PROPERTIES LABELS rt SKIP_RETURN_CODE 77)
    # The alternate stack's restore refused at thread exit (review rounds 29 and 31).
    add_library(diagnostics-altstack-shim MODULE tests/altstack_shim.cpp)
    target_link_libraries(diagnostics-altstack-shim PRIVATE ${CMAKE_DL_LIBS})
    supple_target_defaults(diagnostics-altstack-shim)
    add_test(NAME diagnostics.altstack-restore COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/altstack_faults.py" "$<TARGET_FILE:diagnostics-fixture>" "$<TARGET_FILE:diagnostics-altstack-shim>" "${SUPPLE_SANITIZE}")
    set_tests_properties(diagnostics.altstack-restore PROPERTIES LABELS rt SKIP_RETURN_CODE 77)
  endif()
endif()
