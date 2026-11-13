// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The second half of the factory rack patches played (test_rack_patches.cpp
// plays the first).
#include <doctest/doctest.h>

#include "rack_patches_support.h"

TEST_CASE("factory rack patches: each of the second half loads and plays under full scale") {
    const auto& patches = rack::factoryPatches();
    const std::size_t mid = patches.size() / 2;
    for (std::size_t i = mid; i < patches.size(); ++i) playsUnderFullScale(patches[i]);
}
