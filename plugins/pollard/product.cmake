# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# Pollard, Steppe's stock drum sampler: the DSP script in script/ as a plugin
# of its own, compiled as C++ and run by the suite's glue (plugins/suite) as
# the script player runs it. The plugin is what the script declares
# (Catalog.cpp, from a source export of the script): its parameters, buses
# and identity come from the script, so a session or a preset saved while the
# script ran in the player loads in this one. See plugins/README.md.
#
# PRODUCT_ID and PRODUCT_STATE_TAG are the plugin's own, permanent once
# shipped. The state tag is the one the player gives this ID.

set(PRODUCT_NAME        "Pollard")
set(PRODUCT_ID          "com.suppleaudio.pollard")
set(PRODUCT_VERSION     "1.0.0")
set(PRODUCT_DESCRIPTION "Drum sampler: sixteen pads on the keys from C2, each a sample played one-shot or gated through an envelope and a filter, with choke groups, a little variation per hit and eight extra outputs.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")
set(PRODUCT_CODE        "pollard")

set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "instrument")
set(PRODUCT_CHANNELS    2)
set(PRODUCT_FEATURES    "drum-machine;sampler")
set(PRODUCT_AUX_OUTPUTS "Out 1" "Out 2" "Out 3" "Out 4" "Out 5" "Out 6" "Out 7" "Out 8")

# The suite's layer: the semantic-params extension and the script's status
# in the host's log.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "Plrd")
set(PRODUCT_STATE_TAG       "ntDE")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The automatic editor, laid out from the script's parameters.
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    "")
# The factory kits (samples/LICENSE: CC0, rendered from Marram by
# tools/factory-samples/render.py), inside the plugin: "factory:analog-kick.flac".
# Their names are permanent once shipped, like the parameters' keys.
set(PRODUCT_ASSETS          product/samples/analog-boom.flac product/samples/analog-clap.flac
                            product/samples/analog-cowbell.flac product/samples/analog-cymbal.flac
                            product/samples/analog-hat.flac product/samples/analog-kick.flac
                            product/samples/analog-open-hat.flac product/samples/analog-rim.flac
                            product/samples/analog-shaker.flac product/samples/analog-snare.flac
                            product/samples/analog-tom.flac product/samples/fm-hat.flac
                            product/samples/fm-kick.flac product/samples/fm-open-hat.flac
                            product/samples/fm-ride.flac product/samples/fm-snare.flac
                            product/samples/fm-tom.flac product/samples/fm-zap.flac
                            product/samples/hand-bongo.flac product/samples/hand-cabasa.flac
                            product/samples/hand-clave.flac product/samples/hand-conga.flac
                            product/samples/hand-tambourine.flac
                            product/samples/hand-woodblock.flac)

set(PRODUCT_SMOKE_PLAIN_PARAM   "Pad 1 Level")
set(PRODUCT_SMOKE_LATENCY_PARAM "Oversampling")
