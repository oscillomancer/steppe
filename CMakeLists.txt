# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
cmake_minimum_required(VERSION 3.25)

include(cmake/Product.cmake)
project(${PRODUCT_SLUG} VERSION ${PRODUCT_VERSION} LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SUPPLE_BUILD_FUZZERS "Build parser fuzz targets" OFF)
option(SUPPLE_BUILD_TESTS "Build unit tests" ON)
option(SUPPLE_BUILD_PLUGINS "Build the stock plugins (template-derived)" ON)
option(SUPPLE_BUILD_GUI "Build GUI components" ON)
option(SUPPLE_BUILD_SPIKES "Build experimental research spikes" OFF)
option(SUPPLE_PACKAGE_ONLY "Build only the stock plugin formats shipped in packages" OFF)
if(SUPPLE_PACKAGE_ONLY AND SUPPLE_BUILD_TESTS)
  message(FATAL_ERROR "SUPPLE_PACKAGE_ONLY requires SUPPLE_BUILD_TESTS=OFF; native CI builds the full suite")
endif()

include(cmake/Platforms.cmake)
include(cmake/Defaults.cmake)

if(SUPPLE_BUILD_TESTS)
  enable_testing()
endif()

# Generated identity header: <supple/product.h>
configure_file(cmake/product.h.in ${CMAKE_BINARY_DIR}/generated/supple/product.h @ONLY)
add_library(product-info INTERFACE)
target_include_directories(product-info INTERFACE ${CMAKE_BINARY_DIR}/generated)
add_library(supple::product-info ALIAS product-info)

add_subdirectory(third_party)
# Native device library is DAW-only, never part of isolated plugin builds.
include(cmake/PortAudio.cmake)
add_subdirectory(libs)
add_subdirectory(plugins)
add_subdirectory(modules)
add_subdirectory(host)
add_subdirectory(mcp)
add_subdirectory(daw)
# The build's copy of the agent skill, where host::skillDir() looks in a build tree.
add_custom_target(skill-tree ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_SOURCE_DIR}/skill ${CMAKE_BINARY_DIR}/skill
  COMMENT "Copying the agent skill into the build tree"
  VERBATIM)
add_subdirectory(tests)
# The stock plugins' factory samples' encoder (tools/factory-samples/render.py).
add_subdirectory(tools/factory-samples)
if(SUPPLE_BUILD_SPIKES)
  add_subdirectory(tools/spikes/gui-shell)
endif()
if(SUPPLE_BUILD_FUZZERS)
  add_subdirectory(tests/fuzz)
endif()
if(WIN32)
  supple_windows_utf8_manifests("${PROJECT_SOURCE_DIR}")
endif()

# License gate: fails the build when a dependency is not declared. Python is
# required (building.md lists it on every platform), so the gate is never skipped.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(license-check ALL
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/license-check/check.py --root ${CMAKE_SOURCE_DIR}
  COMMENT "Checking third-party licenses"
  VERBATIM)
if(TARGET package-runtime)
  add_dependencies(package-runtime license-check)
endif()
if(SUPPLE_BUILD_TESTS)
  add_test(NAME license-check
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/license-check/check.py --root ${CMAKE_SOURCE_DIR})
  add_test(NAME license-check.tests
           COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools/license-check)
  add_test(NAME eval-harness.tests
           COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools/eval-harness)
  set_tests_properties(eval-harness.tests PROPERTIES LABELS "eval")
  set(tools listening optimize-bench gui)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND tools reaper-check)  # a Linux-only tool (process groups, shebang fakes)
  endif()
  foreach(tool IN LISTS tools)
    add_test(NAME tools.${tool}.tests
             COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_SOURCE_DIR}/tools/${tool}
                     -p "test_*.py")
  endforeach()
endif()
