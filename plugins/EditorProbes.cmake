# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Runs one plugin's on-screen editor probes (plugin.<name>.embedding), or says
# it skipped them. The probes open a real editor window at a fixed spot on the
# screen, so on a developer's desktop they run only when
# SUPPLE_EDITOR_PROBE_TEST is exactly 1; native CI sets it on every platform
# (Linux under Xvfb). Run with:
#   cmake -DCTEST=<ctest> -DTEST_DIR=<plugin build> -DREGEX=<probes> -P EditorProbes.cmake
# The test's SKIP_REGULAR_EXPRESSION matches the line printed below, so ctest
# reports "Skipped", not "Passed".
if(NOT "$ENV{SUPPLE_EDITOR_PROBE_TEST}" STREQUAL "1")
  message(STATUS "editor probes skipped: set SUPPLE_EDITOR_PROBE_TEST=1 to open them on this screen")
  return()
endif()
execute_process(
  COMMAND "${CTEST}" --test-dir "${TEST_DIR}" --output-on-failure -R "${REGEX}" --no-tests=ignore
  RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "editor probes failed (${result})")
endif()
