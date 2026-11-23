// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The names the operating system shows for this process's threads (what a
// debugger, a profiler or a crash report lists), read natively for tests:
// /proc/self/task/*/comm on Linux, Mach's extended thread info on macOS,
// GetThreadDescription on Windows. Defined in native_threads.cpp, so the
// platform headers stay out of the test files.
#pragma once

#include <set>
#include <string>

namespace supple::test {
std::set<std::string> osThreadNames();
}  // namespace supple::test
