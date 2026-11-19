# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Offline crash resolver (ADR-0083, topic 4): names functions in a local crash report from
# symbol files on this machine. Separate from the diagnostics library on purpose: the
# application never links it.
supple_add_library(crash-resolver SOURCES src/image.cpp src/resolve.cpp
  DEPS supple::core-base supple::core-json supple::image-identity)
add_executable(resolve-crash tools/resolve_crash.cpp)
target_link_libraries(resolve-crash PRIVATE supple::crash-resolver)
supple_target_defaults(resolve-crash)
supple_add_tests(crash-resolver SOURCES tests/test_image.cpp tests/test_resolve.cpp)
if(SUPPLE_BUILD_TESTS)
  # The synthetic ELF, Mach-O and PE builders and the scratch folder are image-identity's test
  # helpers, shared with these tests.
  target_include_directories(crash-resolver.tests PRIVATE "${PROJECT_SOURCE_DIR}/libs/image-identity/tests")
endif()
