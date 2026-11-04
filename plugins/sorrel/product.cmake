# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Sorrel, Steppe's stock gate: the DSP script in script/ as a plugin
# of its own, compiled as C++ and run by the suite's glue (plugins/suite) as
# the script player runs it. The plugin is what the script declares
# (Catalog.cpp, from a source export of the script): its parameters, buses
# and identity come from the script, so a session or a preset saved while the
# script ran in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Sorrel")
set(PRODUCT_ID          "com.suppleaudio.sorrel")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "Gate and expander: lookahead that opens exactly on the onset, fades that follow the sound's own decay, a key input, and a level histogram that suggests the threshold.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "sorrel")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "effect")
set(PRODUCT_CHANNELS    2 1)
set(PRODUCT_FEATURES    "gate")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Sorl")
set(PRODUCT_STATE_TAG       "sTMn")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The script draws its own editor (script_editor.cpp, from the suite).
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Threshold")
set(PRODUCT_SMOKE_LATENCY_PARAM "Lookahead")
