# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(mcp SOURCES src/server.cpp src/native_writer.cpp DEPS supple::control-protocol supple::product-info)
add_executable(${PRODUCT_SLUG}-mcp src/proxy_main.cpp)
target_link_libraries(${PRODUCT_SLUG}-mcp PRIVATE supple::mcp)
supple_target_defaults(${PRODUCT_SLUG}-mcp)
if(SUPPLE_BUILD_TESTS)
  supple_add_tests(mcp SOURCES tests/test_mcp.cpp)
endif()
