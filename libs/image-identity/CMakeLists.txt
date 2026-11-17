# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# GPL, deliberately: which executable image a file is and its build id (ADR-0083). It moved
# here unchanged from the crash resolver's image reader so that the application can compare
# an installed plug-in file with a crash report's build id (topic 7d) without linking the
# resolver's symbol readers. DAW-only: nothing under plugins/, modules/ or the MIT
# libraries may link it: tools/license-check rejects any mention of it in their files, and
# cmake/LicenseIsolation.cmake fails configuration when one of their targets reaches it.
# `supple/imageid/parse.h` is internal to this library and the crash resolver.
supple_add_library(image-identity SOURCES src/image.cpp DEPS supple::core-base)
