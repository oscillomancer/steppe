# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# GPL, deliberately: it is DAW-only crash-report plumbing (ADR-0083). Nothing under
# plugins/, modules/ or the MIT libraries may link it: tools/license-check rejects any
# mention of it in their files, and cmake/LicenseIsolation.cmake fails configuration when
# one of their targets reaches it (add a new GPL-only library to both).
supple_add_library(diagnostics-context SOURCES src/table.cpp src/scope.cpp src/inventory.cpp src/commands.cpp DEPS supple::core-base)
supple_add_tests(diagnostics-context SOURCES tests/test_table.cpp tests/test_scope.cpp tests/test_commands.cpp "${PROJECT_SOURCE_DIR}/tests/progress_listener.cpp" LABELS rt)
if(SUPPLE_BUILD_TESTS)
  # Names each case on stderr as it starts, and any that runs long: a hang on a runner is
  # then a named case in the log, not a silent timeout.
  target_compile_definitions(diagnostics-context.tests PRIVATE PROGRESS_SUITE="diagnostics-context.tests")
endif()
