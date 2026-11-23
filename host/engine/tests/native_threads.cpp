// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "native_threads.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// After <windows.h>, which it needs.
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <filesystem>
#include <fstream>
#endif

namespace supple::test {

std::set<std::string> osThreadNames() {
    std::set<std::string> names;
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return names;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD self = GetCurrentProcessId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self) continue;
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        PWSTR wide = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide) {
            const int bytes =
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 1) {
                std::string name(static_cast<std::size_t>(bytes - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), bytes, nullptr, nullptr);
                names.insert(name);
            }
            LocalFree(wide);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
#elif defined(__APPLE__)
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return names;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        thread_extended_info_data_t info{};
        mach_msg_type_number_t size = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(threads[i], THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info),
                        &size) == KERN_SUCCESS &&
            info.pth_name[0] != '\0')
            names.insert(info.pth_name);
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  count * sizeof(thread_act_t));
#else
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end;
         it.increment(ec)) {
        std::ifstream comm(it->path() / "comm");
        std::string name;
        if (std::getline(comm, name)) names.insert(name);
    }
#endif
    return names;
}

}  // namespace supple::test
