# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(notation SOURCES src/notation.cpp src/decompile.cpp DEPS supple::midi supple::harmony supple::core-base)
supple_add_tests(notation SOURCES tests/test_notation.cpp)
if(SUPPLE_BUILD_TESTS)
  target_compile_definitions(notation.tests PRIVATE NOTATION_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/corpus")
endif()

add_executable(notation-ref tools/notation_ref.cpp)
target_include_directories(notation-ref PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(notation-ref PRIVATE supple::notation)
supple_target_defaults(notation-ref)

if(SUPPLE_BUILD_TESTS)
  add_test(NAME notation.reference-drift
    COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:notation-ref>
            -DREFERENCE=${PROJECT_SOURCE_DIR}/skill/generated/notation-reference.md
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_reference.cmake)
endif()
