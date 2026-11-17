# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 Supple Audio
# GPL, deliberately: it is DAW-only crash-report plumbing (ADR-0083). Nothing under
# plugins/, modules/ or the MIT libraries may link it; tools/license-check enforces that
# by leaving it out of its MIT list.
supple_add_library(diagnostics-context SOURCES src/table.cpp)
supple_add_tests(diagnostics-context SOURCES tests/test_table.cpp LABELS rt)
