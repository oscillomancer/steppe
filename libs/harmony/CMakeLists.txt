# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
supple_add_library(harmony SOURCES src/harmony.cpp DEPS supple::core-base)
supple_add_tests(harmony SOURCES tests/test_harmony.cpp)
