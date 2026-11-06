#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerates the committed generator output under skill/generated/ from the
# dev build (checked by the sandbox.registry-drift test).
set -euo pipefail
cd "$(dirname "$0")/../.."
slug=$(sed -n 's/^set(PRODUCT_SLUG "\(.*\)").*/\1/p' cmake/Product.cmake)
tool=build/dev/host/sandbox/${slug}-sandbox
cmake --build --preset dev --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target "${slug}-sandbox" >/dev/null
"$tool" registry --markdown > skill/generated/sandbox-commands.md
"$tool" registry --mcp-tools > skill/generated/sandbox-mcp-tools.json
"$tool" registry --lua > skill/generated/sandbox.lua
engine=build/dev/host/engine/${slug}-engine
cmake --build --preset dev --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target "${slug}-engine" >/dev/null
"$engine" registry --markdown > skill/generated/engine-commands.md
"$engine" registry --mcp-tools > skill/generated/engine-mcp-tools.json
"$engine" registry --lua > skill/generated/engine.lua
"$engine" modules --markdown > skill/generated/rack-modules.md
cmake --build --preset dev --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-2}" --target notation-ref >/dev/null
build/dev/libs/notation/notation-ref > skill/generated/notation-reference.md
cmake -DEXAMPLES=plugins/suite/examples -DOUT=skill/generated/dsp-examples.md -P tools/gen/dsp_examples.cmake
echo "updated skill/generated/{sandbox,engine}-{commands.md,mcp-tools.json,lua}, rack-modules.md, notation-reference.md and dsp-examples.md"
