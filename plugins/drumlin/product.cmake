# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Drumlin, Steppe's stock drum machine: the DSP script in script/ as a plugin
# of its own, compiled as C++ and run by the suite's glue (plugins/suite) as
# the script player runs it. The plugin is what the script declares
# (Catalog.cpp, from a source export of the script): its parameters, buses
# and identity come from the script, so a session or a preset saved while the
# script ran in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Drumlin")
set(PRODUCT_ID          "com.suppleaudio.drumlin")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "Drum machine: classic machines' sounds made anew without their samples, the LM-1's first, on sixteen pads from C2 with choke groups and eight extra outputs.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "drumlin")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "instrument")
set(PRODUCT_CHANNELS    2)
set(PRODUCT_FEATURES    "drum-machine;synthesizer")
set(PRODUCT_AUX_OUTPUTS "Out 1" "Out 2" "Out 3" "Out 4" "Out 5" "Out 6" "Out 7" "Out 8")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Drml")
set(PRODUCT_STATE_TAG       "WkjN")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The script draws its own editor (script_editor.cpp, from the suite).
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    "")

set(PRODUCT_SMOKE_PLAIN_PARAM   "Pad 1 Level")
set(PRODUCT_SMOKE_LATENCY_PARAM "")
