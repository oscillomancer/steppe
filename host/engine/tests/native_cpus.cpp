// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "native_cpus.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

namespace supple::test {

namespace {
bool allow([[maybe_unused]] const std::vector<int>& cpus) {
#if defined(_WIN32)
    DWORD_PTR mask = 0;
    for (int c : cpus) mask |= DWORD_PTR{1} << c;
    return mask != 0 && SetProcessAffinityMask(GetCurrentProcess(), mask) != 0;
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus) CPU_SET(c, &set);
    return !cpus.empty() && ::sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    return false;
#endif
}
}  // namespace

std::vector<int> allowedCpuList() {
    std::vector<int> out;
#if defined(_WIN32)
    DWORD_PTR process = 0, system = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process, &system)) return out;
    for (int c = 0; c < static_cast<int>(sizeof(DWORD_PTR) * 8); ++c)
        if (process & (DWORD_PTR{1} << c)) out.push_back(c);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(0, sizeof(set), &set) != 0) return out;
    for (int c = 0; c < CPU_SETSIZE; ++c)
        if (CPU_ISSET(c, &set)) out.push_back(c);
#endif
    return out;
}

CpuRestriction::CpuRestriction(const std::vector<int>& cpus, std::size_t n)
    : before_(allowedCpuList()) {
    if (n == 0 || n > cpus.size()) return;
    ok_ = allow(std::vector<int>(cpus.begin(), cpus.begin() + static_cast<std::ptrdiff_t>(n)));
}

CpuRestriction::~CpuRestriction() {
    if (ok_) (void)allow(before_);
}

}  // namespace supple::test
