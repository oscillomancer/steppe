# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Compares the sandbox's (or the engine's) generator output with the files
# committed under skill/generated/. Regenerate with tools/gen/update-generated.sh.
if(NOT DEFINED PREFIX)
  set(PREFIX sandbox)
endif()
foreach(pair "--markdown;${PREFIX}-commands.md" "--mcp-tools;${PREFIX}-mcp-tools.json"
             "--lua;${PREFIX}.lua")
  list(GET pair 0 flag)
  list(GET pair 1 file)
  execute_process(COMMAND ${TOOL} registry ${flag} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${TOOL} registry ${flag} failed (${rc})")
  endif()
  file(READ ${GENERATED}/${file} committed)
  if(NOT out STREQUAL committed)
    message(FATAL_ERROR "skill/generated/${file} is out of date: run tools/gen/update-generated.sh")
  endif()
endforeach()
# The engine's also prints the built-in rack modules (rack-modules.md).
if(PREFIX STREQUAL "engine")
  execute_process(COMMAND ${TOOL} modules --markdown OUTPUT_VARIABLE out RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${TOOL} modules --markdown failed (${rc})")
  endif()
  file(READ ${GENERATED}/rack-modules.md committed)
  if(NOT out STREQUAL committed)
    message(FATAL_ERROR "skill/generated/rack-modules.md is out of date: run tools/gen/update-generated.sh")
  endif()
endif()
