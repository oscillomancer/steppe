# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# GPL, deliberately: the crash-report capture code (ADR-0083): the wire packets, the trace
# capsule and its parser, the frame-pointer walker and the per-platform stack readers. It
# was part of host/diagnostics and moved here unchanged so that a process that only has to
# capture its own failure (the bridge child, topic 6) can link it without the supervisor,
# the frontend or the report store. DAW-only: nothing under plugins/, modules/ or the MIT
# libraries may link it: tools/license-check rejects any mention of it in their files, and
# cmake/LicenseIsolation.cmake fails configuration when one of their targets reaches it.
# The headers are internal to the diagnostics code (namespace supple::diagnostics::detail).
if(WIN32)
  set(capture_source src/trace_windows.cpp src/self_capture_windows.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(capture_source src/capsule_posix.cpp src/altstack_posix.cpp src/trace_linux.cpp
    src/self_capture_posix.cpp src/self_capture_linux.cpp)
elseif(APPLE)
  set(capture_source src/capsule_posix.cpp src/altstack_posix.cpp src/trace_other.cpp
    src/capture_macos.cpp src/self_capture_posix.cpp src/self_capture_macos.cpp)
else()
  set(capture_source src/capsule_posix.cpp src/altstack_posix.cpp src/trace_other.cpp)
endif()
supple_add_library(diagnostics-capture SOURCES src/trace.cpp src/self_capture.cpp ${capture_source}
  DEPS supple::core-base supple::core-json supple::diagnostics-context)
if(WIN32)
  target_link_libraries(diagnostics-capture PRIVATE dbghelp psapi)
endif()
# The capture code's own tests: the walker, module identity, the capsule and its strict parser.
# The progress listener names each case as it starts, as the diagnostics tests do.
supple_add_tests(diagnostics-capture SOURCES tests/test_trace.cpp tests/test_capsule.cpp tests/test_selfcapture.cpp
  "${PROJECT_SOURCE_DIR}/tests/progress_listener.cpp")
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(diagnostics-capture.tests PRIVATE PROGRESS_SUITE="diagnostics-capture.tests")
endif()
