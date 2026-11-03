# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
supple_add_library(audio-file
  SOURCES src/audio_file.cpp src/flac_encode.cpp src/resample.cpp src/wav_stream.cpp src/inspect.cpp src/metadata.cpp src/dr_impl.c
  DEPS supple::core-base supple::core-dsp
  PRIVATE_DEPS supple::dr-libs)
# Third-party code: no warnings for the dr_libs translation unit.
set_source_files_properties(src/dr_impl.c PROPERTIES COMPILE_OPTIONS "-w")

supple_add_tests(audio-file SOURCES tests/test_audio_file.cpp)
