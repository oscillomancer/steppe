# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${SOURCE_DIR}"
  OUTPUT_VARIABLE diagnostics_commit OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET RESULT_VARIABLE diagnostics_git)
execute_process(COMMAND git status --porcelain --untracked-files=normal WORKING_DIRECTORY "${SOURCE_DIR}"
  OUTPUT_VARIABLE diagnostics_status ERROR_QUIET)
if(diagnostics_git)
  set(diagnostics_commit unknown)
endif()
if(diagnostics_git OR diagnostics_status)
  set(diagnostics_dirty true)
else()
  set(diagnostics_dirty false)
endif()
configure_file("${SOURCE_DIR}/host/diagnostics/src/diagnostics_build.h.in"
  "${OUTPUT_DIR}/diagnostics_build.h" @ONLY)
