# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Marram, Steppe's stock synthesizer: the DSP script in script/ as a plugin of
# its own, compiled as C++ and run by the suite's glue (plugins/suite) as the
# script player runs it. The plugin is what the script declares (Catalog.cpp,
# from a source export of the script): its parameters, buses and identity
# come from the script, so a session or a preset saved while the script ran
# in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Marram")
set(PRODUCT_ID          "com.suppleaudio.marram")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "Synthesizer: four oscillators (analog, wavetable, FM, noise, sub) with unison, two filters (ladder, state-variable, comb, formant), three envelopes, three LFOs and four macros, connected by modulation routes.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "marram")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "instrument")
set(PRODUCT_CHANNELS    2)
set(PRODUCT_FEATURES    "synthesizer")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Mrrm")
set(PRODUCT_STATE_TAG       "XZXx")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The automatic editor, laid out from the script's parameters.
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    "")

set(PRODUCT_SMOKE_PLAIN_PARAM   "Osc 1 Level")
set(PRODUCT_SMOKE_LATENCY_PARAM "Oversampling")
