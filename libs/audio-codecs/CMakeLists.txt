# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# Lossy render encoders (ADR-0055): MP3 through LAME (LGPL, DAW-only) and Ogg
# Vorbis through libvorbis, plus the Vorbis decoder the delivery checks use.
# Linked only by the engine: never by plugins/ or the MIT libraries.
supple_add_library(audio-codecs
  SOURCES src/mp3.cpp src/vorbis.cpp
  DEPS supple::audio-file supple::core-base
  PRIVATE_DEPS supple::lame supple::vorbis)

supple_add_tests(audio-codecs SOURCES tests/test_codecs.cpp)
