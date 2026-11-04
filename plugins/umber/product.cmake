# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Umber, Steppe's stock tape machine: the DSP script in script/ as a plugin of
# its own, compiled as C++ and run by the suite's glue (plugins/suite) as the
# script player runs it. The plugin is what the script declares (Catalog.cpp,
# from a source export of the script): its parameters, buses and identity
# come from the script, so a session or a preset saved while the script ran
# in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Umber")
set(PRODUCT_ID          "com.suppleaudio.umber")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "A tape machine modelled from the oxide up: class-A electronics, record equalisation, a magnetic core whose hysteresis follows the bias, the playback head's bump and losses, wow, flutter and scrape, dropouts, print-through, crosstalk, hiss and hum, at four speeds, from a faint rounding of peaks to a worn, warbling cassette.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "umber")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "effect")
set(PRODUCT_CHANNELS    2 1)
set(PRODUCT_FEATURES    "distortion;tape")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Umbr")
set(PRODUCT_STATE_TAG       "auff")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The script draws its own editor (script_editor.cpp, from the suite).
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    "")

# Drive: its knob sits at the editor's left, where a small test screen
# still shows it.
set(PRODUCT_SMOKE_PLAIN_PARAM   "Drive")
set(PRODUCT_SMOKE_LATENCY_PARAM "Latency")
