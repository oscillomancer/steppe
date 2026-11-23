// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// CPU affinity for tests, where the engine reads it (core-base
// allowedCpuCount): the calling thread's mask on Linux, the process's on
// Windows. macOS has no CPU affinity: the list is empty and nothing restricts.
// Defined in native_cpus.cpp, so <windows.h> stays out of the test files.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace supple::test {

// The CPUs allowed now (empty on macOS or on failure).
std::vector<int> allowedCpuList();

// Allows only the first `n` of `cpus` for the scope: the calling thread on
// Linux, the whole process on Windows. The mask before is restored at the end.
class CpuRestriction {
  public:
    CpuRestriction(const std::vector<int>& cpus, std::size_t n);
    ~CpuRestriction();
    CpuRestriction(const CpuRestriction&) = delete;
    CpuRestriction& operator=(const CpuRestriction&) = delete;
    bool ok() const { return ok_; }

  private:
    bool ok_ = false;
    std::vector<int> before_;
};

}  // namespace supple::test
