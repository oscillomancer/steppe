# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# The suite (Steppe's plugins/suite: suite.cmake), and the script compiled as
# the script player's compiler runs it: integers wrap, and no fused
# multiply-adds. An Array may start from single braces (= {1.0, 0.5}), as
# the language allows, without a warning.

get_filename_component(SUPPLE_SUITE "${SUPPLE_PRODUCT}/../suite" ABSOLUTE)
include(${SUPPLE_SUITE}/suite.cmake)
if (MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(${SUPPLE_PRODUCT}/Catalog.cpp PROPERTIES
        COMPILE_OPTIONS "/clang:-ffp-contract=off;/clang:-fwrapv;-Wno-missing-braces")
elseif (NOT MSVC)
    set_source_files_properties(${SUPPLE_PRODUCT}/Catalog.cpp PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off;-fwrapv;-Wno-missing-braces")
endif()
