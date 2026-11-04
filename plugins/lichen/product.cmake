# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Lichen, Steppe's stock analyser: the DSP script in script/ as a plugin of
# its own, compiled as C++ and run by the suite's glue (plugins/suite) as the
# script player runs it. The plugin is what the script declares (Catalog.cpp,
# from a source export of the script): its parameters, buses and identity
# come from the script, so a session or a preset saved while the script ran
# in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Lichen")
set(PRODUCT_ID          "com.suppleaudio.lichen")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "An analyser that passes every sample untouched: loudness to BS.1770 and EBU R128, true peak, stereo, spectrum and key, shown in its editor and reported as readouts for agents.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "lichen")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "effect")
set(PRODUCT_CHANNELS    2 1)
set(PRODUCT_FEATURES    "analyzer")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Lchn")
set(PRODUCT_STATE_TAG       "cwHJ")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The script draws its own editor (script_editor.cpp, from the suite).
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    product/test/restart_test.cpp)

# Target moves only the editor's reference line; the audio passes through
# exactly, which host-smoke checks of a transparent product.
set(PRODUCT_SMOKE_PLAIN_PARAM   "Target")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
set(PRODUCT_TRANSPARENT         ON)
