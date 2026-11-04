# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Supple Audio
# The player: the plugin that runs DSP scripts (Steppe's ADR-0032). Its
# plugins are the scripts it finds (product/Catalog.cpp); it is a run-time
# product of the plugin template with the suite's layer. It builds from the
# template's tree with this folder as the product
# (-DSUPPLE_PRODUCT_DIR=<this folder>); the suite is the folder above.
#
# PRODUCT_ID and PRODUCT_STATE_TAG name the module, not its plugins: each
# script's plugin has its own ID and a state tag made from it.

set(PRODUCT_NAME        "Script Player")
set(PRODUCT_ID          "com.suppleaudio.script-player")
set(PRODUCT_VERSION     "0.1.0")
set(PRODUCT_DESCRIPTION "Runs DSP scripts as plugins.")
set(PRODUCT_VENDOR      "Supple Audio")
set(PRODUCT_URL         "")
set(PRODUCT_COPYRIGHT   "Copyright (c) 2026 Supple Audio")

# What its plugins are is decided when it runs. The kind and layouts below
# are those of the script host-smoke tests (test/library/smoke).
set(PRODUCT_RUNTIME     ON)
set(PRODUCT_PRESETS     ON)
set(PRODUCT_KIND        "effect")
set(PRODUCT_CHANNELS    2 1)
set(PRODUCT_FEATURES    "utility")

# The suite's layer: the semantic-params extension (targets.cmake adds the
# suite's half), and scripts reloaded.
set(PRODUCT_LAYER           ON)
set(PRODUCT_LAYER_SOURCES   product/Layer.cpp)

set(PRODUCT_AU_MANUFACTURER "Supl")
set(PRODUCT_AU_SUBTYPE      "ScPl")
set(PRODUCT_STATE_TAG       "ScPl")

set(PRODUCT_DSP_SOURCES     product/Catalog.cpp)
# The automatic editor, laid out from each script's parameters.
set(PRODUCT_EDITOR_SOURCES  "")
set(PRODUCT_TEST_SOURCES    product/test/processor_test.cpp)

set(PRODUCT_SMOKE_PLAIN_PARAM "Gain")
