# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Shared core libraries. Order matters only for readability; CMake resolves
# targets lazily.
add_subdirectory(core-base)
add_subdirectory(core-json)
add_subdirectory(core-schema)
add_subdirectory(core-params)
add_subdirectory(core-patch)
add_subdirectory(command-bus)
add_subdirectory(lua-bus)
add_subdirectory(core-dsp)
add_subdirectory(console9)
add_subdirectory(control-protocol)
add_subdirectory(audio-file)
add_subdirectory(audio-codecs)
add_subdirectory(core-analysis)
add_subdirectory(midi)
add_subdirectory(notation)
add_subdirectory(timeline)
add_subdirectory(optimize)
