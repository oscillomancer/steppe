// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include "../src/native.h"
using namespace supple::diagnostics::detail;
TEST_CASE("Windows acknowledged return values are not exception evidence") {
    CHECK_FALSE(windowsFatalStatus(0xffffffffU, 0xffffffffU));
    CHECK_FALSE(windowsFatalStatus(0xc0000005U, 0xc0000005U));
    CHECK_FALSE(windowsFatalStatus(7, 7));
    CHECK(windowsFatalStatus(0xc0000005U, 7));
    CHECK(windowsFatalStatus(0xc0000005U, std::nullopt));
    CHECK_FALSE(windowsFatalStatus(7, std::nullopt));
}
