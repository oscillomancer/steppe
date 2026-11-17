# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(control-protocol SOURCES src/protocol.cpp src/model_loop.cpp
  DEPS supple::command-bus supple::product-info)
if(WIN32)
  target_link_libraries(control-protocol PRIVATE ws2_32)
endif()

supple_add_tests(control-protocol SOURCES tests/test_protocol.cpp tests/test_platform.cpp tests/test_limits.cpp)
