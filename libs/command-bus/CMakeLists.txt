# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(command-bus SOURCES src/bus.cpp src/jobs.cpp DEPS supple::core-schema supple::product-info supple::diagnostics-context)
supple_add_tests(command-bus SOURCES tests/test_bus.cpp)
