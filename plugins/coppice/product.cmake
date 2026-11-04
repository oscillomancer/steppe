# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Coppice, Steppe's stock sampler: the DSP script in script/ as a plugin of
# its own, compiled as C++ and run by the suite's glue (plugins/suite) as the
# script player runs it. The plugin is what the script declares (Catalog.cpp,
# from a source export of the script): its parameters, buses and identity
# come from the script, so a session or a preset saved while the script ran
# in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Coppice")
set(PRODUCT_ID          "com.suppleaudio.coppice")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "Sampler: one sound, played pitched with a crossfaded or ping-pong loop, as a one-shot, or cut into slices across the keys, through a filter, two envelopes, two LFOs and four macros, connected by modulation routes.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "coppice")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "instrument")
set(PRODUCT_CHANNELS    2)
set(PRODUCT_FEATURES    "sampler")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Cpce")
set(PRODUCT_STATE_TAG       "Idrr")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The automatic editor, laid out from the script's parameters.
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    "")
# The factory samples (samples/LICENSE: CC0, rendered from Marram by
# tools/factory-samples/render.py), inside the plugin: "factory:keys.flac".
# Their names are permanent once shipped, like the parameters' keys.
set(PRODUCT_ASSETS          product/samples/keys.flac product/samples/pad.flac
                            product/samples/pluck.flac product/samples/bass.flac
                            product/samples/bell.flac product/samples/voice.flac
                            product/samples/break.flac)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Gain")
set(PRODUCT_SMOKE_LATENCY_PARAM "Oversampling")
