# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(model
  SOURCES src/audio_warp.cpp src/item_edits.cpp src/heal.cpp src/freeze.cpp src/session.cpp src/schemas.cpp src/strips.cpp src/migration.cpp src/versions.cpp src/groups.cpp src/render_settings.cpp
  DEPS supple::core-base supple::core-json supple::core-schema supple::midi supple::timeline)
if(SUPPLE_BUILD_TESTS)
  supple_add_tests(model SOURCES tests/test_audio_warp.cpp tests/test_freeze.cpp tests/test_model.cpp tests/test_versions.cpp tests/test_groups.cpp)
endif()
