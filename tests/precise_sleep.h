// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <chrono>

#include "supple/base/sleep.h"

namespace supple::test {

// A pause for a test thread that paces a fake device or sender: a thin name for
// supple::sleepFor (libs/core-base), which on Windows ends on a thread-owned
// high-resolution timer instead of the 15.6 ms system tick (engine-plan-soak.tests
// took 932 s on Windows CI against 119 s on Linux before). For pacing only: no test
// may decide pass or fail by how long this took.
inline void sleepFor(std::chrono::duration<double> duration) { supple::sleepFor(duration); }

}  // namespace supple::test
