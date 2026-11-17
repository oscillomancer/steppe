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
  set(capture_source src/trace_windows.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(capture_source src/capsule_posix.cpp src/trace_linux.cpp)
elseif(APPLE)
  set(capture_source src/capsule_posix.cpp src/trace_other.cpp src/capture_macos.cpp)
else()
  set(capture_source src/capsule_posix.cpp src/trace_other.cpp)
endif()
supple_add_library(diagnostics-capture SOURCES src/trace.cpp ${capture_source}
  DEPS supple::core-base supple::core-json supple::diagnostics-context)
if(WIN32)
  target_link_libraries(diagnostics-capture PRIVATE dbghelp psapi)
endif()
