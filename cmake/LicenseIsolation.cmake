# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
#
# The permissive part of the tree (plugins/, modules/ and the MIT libraries) must not link
# a GPL-only library (ADR-0033, ADR-0062, ADR-0083). tools/license-check/check.py rejects
# an include of such a library's headers or a mention of its target in those files;
# this is the other half: at configure time it walks the link graph of every target built
# from those directories and fails when one reaches a GPL-only library, directly or through
# another target.
#
# SUPPLE_MIT_LIBRARIES is the same list as MIT_LIBRARIES in tools/license-check/check.py
# (a test there keeps them equal). Add a GPL-only library's target name to
# SUPPLE_GPL_ONLY_TARGETS when it is created.
set(SUPPLE_MIT_LIBRARIES core-base core-json core-schema core-params core-patch core-dsp
                         core-analysis)
set(SUPPLE_GPL_ONLY_TARGETS diagnostics-context)

# Every target created in `directory` and below.
function(supple_directory_targets directory result)
  get_property(found DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    supple_directory_targets("${child}" below)
    list(APPEND found ${below})
  endforeach()
  set(${result} "${found}" PARENT_SCOPE)
endfunction()

# The GPL-only target `start` reaches through its link libraries, or empty.
function(supple_reaches_gpl start result)
  set(result_value "")
  set(todo "${start}")
  set(seen "")
  while(todo)
    list(POP_FRONT todo item)
    if(item IN_LIST seen)
      continue()
    endif()
    list(APPEND seen "${item}")
    if(NOT TARGET "${item}")
      continue()
    endif()
    get_target_property(real "${item}" ALIASED_TARGET)
    if(real)
      set(item "${real}")
    endif()
    if(item IN_LIST SUPPLE_GPL_ONLY_TARGETS)
      set(result_value "${item}")
      break()
    endif()
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(libraries "${item}" ${property})
      if(libraries)
        foreach(library IN LISTS libraries)
          # Generator expressions are not evaluated; any name inside one that is a target
          # is followed, which can only over-report.
          string(REGEX MATCHALL "[A-Za-z0-9_.+-]+(::[A-Za-z0-9_.+-]+)*" names "${library}")
          list(APPEND todo ${names})
        endforeach()
      endif()
    endforeach()
  endwhile()
  set(${result} "${result_value}" PARENT_SCOPE)
endfunction()

function(supple_check_license_isolation)
  supple_directory_targets("${PROJECT_SOURCE_DIR}" all_targets)
  set(permissive "${PROJECT_SOURCE_DIR}/plugins" "${PROJECT_SOURCE_DIR}/modules")
  foreach(library IN LISTS SUPPLE_MIT_LIBRARIES)
    list(APPEND permissive "${PROJECT_SOURCE_DIR}/libs/${library}")
  endforeach()
  foreach(target IN LISTS all_targets)
    get_target_property(type "${target}" TYPE)
    if(type STREQUAL "UTILITY")
      continue()
    endif()
    get_target_property(source_dir "${target}" SOURCE_DIR)
    set(is_permissive FALSE)
    foreach(root IN LISTS permissive)
      string(FIND "${source_dir}/" "${root}/" at)
      if(at EQUAL 0)
        set(is_permissive TRUE)
      endif()
    endforeach()
    if(NOT is_permissive)
      continue()
    endif()
    supple_reaches_gpl("${target}" reached)
    if(reached)
      message(FATAL_ERROR
        "license isolation: '${target}' (${source_dir}) links the GPL-only library "
        "'${reached}', directly or through another target. Plug-ins, modules and the MIT "
        "libraries stay MIT (AGENTS.md rule 1).")
    endif()
  endforeach()
endfunction()
