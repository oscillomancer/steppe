# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Timecode, MIDI clock and MTC, LTC and the chase tracker (ADR-0070). GPL and
# DAW-only: it links libltc (LGPL-3.0-or-later), so no plug-in, module or MIT
# library may link it.
supple_add_library(sync SOURCES src/common.cpp src/timecode.cpp src/midi_clock.cpp src/mtc.cpp
                   src/ltc.cpp src/chase.cpp
                   DEPS supple::core-base PRIVATE_DEPS supple::ltc)
supple_add_tests(sync SOURCES tests/test_timecode.cpp tests/test_midi_clock.cpp tests/test_mtc.cpp
                 tests/test_ltc.cpp tests/test_chase.cpp LABELS rt SPLIT)
