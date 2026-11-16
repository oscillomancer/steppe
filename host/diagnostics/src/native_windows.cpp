// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "native.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>
#include <io.h>
#include <shellapi.h>
#include <atomic>
#include <cstdio>
#include <new>
namespace supple::diagnostics::detail {
namespace {
struct Shared {
    Packet boot;
    std::atomic<std::uint32_t> ready{0};
    Packet messages[3];  // immutable after each release publication
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
HANDLE handle(std::intptr_t value) { return reinterpret_cast<HANDLE>(value); }
std::intptr_t resource(HANDLE value) { return reinterpret_cast<std::intptr_t>(value); }
std::wstring quoted(std::wstring_view text) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
Error failure() {
    return Error("diagnostics.bootstrap", "cannot prepare private supervision", {},
                 "continue without diagnostics; check process resource limits");
}
}  // namespace
std::uint64_t pid() { return GetCurrentProcessId(); }
Result<std::string> incidentId() {
    std::array<unsigned char, 16> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return failure();
    std::string id;
    for (auto x : random) {
        id += "0123456789abcdef"[x >> 4];
        id += "0123456789abcdef"[x & 15];
    }
    return id;
}
Result<Native> launch(int, char**, const Packet& boot) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Native n;
    n.channel = resource(CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
                                            sizeof(Shared), nullptr));
    n.event = resource(CreateEventW(&security, FALSE, FALSE, nullptr));
    n.ack = resource(CreateEventW(&security, FALSE, FALSE, nullptr));
    if (!handle(n.channel) || !handle(n.event) || !handle(n.ack)) {
        close(n);
        return failure();
    }
    n.shared = MapViewOfFile(handle(n.channel), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!n.shared) {
        close(n);
        return failure();
    }
    auto* shared = new (n.shared) Shared;
    shared->boot = boot;
    HANDLE parent = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &parent,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
        close(n);
        return failure();
    }
    std::vector<HANDLE> inherited{handle(n.channel), handle(n.event), handle(n.ack), parent};
    const DWORD streams[]{STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
    for (DWORD stream : streams) {
        HANDLE copy = nullptr;
        const HANDLE source = GetStdHandle(stream);
        if (!source || source == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE,
                             DUPLICATE_SAME_ACCESS))
            copy = CreateFileW(L"NUL", stream == STD_INPUT_HANDLE ? GENERIC_READ : GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0,
                               nullptr);
        if (copy == INVALID_HANDLE_VALUE) {
            for (std::size_t i = 4; i < inherited.size(); ++i) CloseHandle(inherited[i]);
            CloseHandle(parent);
            close(n);
            return failure();
        }
        inherited.push_back(copy);
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> memory(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    bool initialized = InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) != FALSE;
    bool configured = initialized &&
                      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                inherited.data(), inherited.size() * sizeof(HANDLE),
                                                nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = inherited[4];
    startup.StartupInfo.hStdOutput = inherited[5];
    startup.StartupInfo.hStdError = inherited[6];
    startup.lpAttributeList = attributes;
    std::array<wchar_t, 32768> executable{};
    const DWORD length =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    int argumentCount = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    const bool parsed = arguments && argumentCount > 0;
    std::wstring command;
    if (arguments) {
        for (int i = 0; i < argumentCount; ++i) {
            if (i) command += L' ';
            command += quoted(arguments[i]);
        }
        LocalFree(arguments);
    }
    command += L" --diagnostic-worker " + std::to_wstring(n.channel) + L":" +
               std::to_wstring(n.event) + L":" + std::to_wstring(n.ack) + L":" +
               std::to_wstring(resource(parent));
    PROCESS_INFORMATION process{};
    const bool created = parsed && !command.empty() && configured && length > 0 &&
                         length < executable.size() &&
                         CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE,
                                        EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                        &startup.StartupInfo, &process);
    if (initialized) DeleteProcThreadAttributeList(attributes);
    for (std::size_t i = 4; i < inherited.size(); ++i) CloseHandle(inherited[i]);
    CloseHandle(parent);
    if (!created) {
        close(n);
        return failure();
    }
    n.process = resource(process.hProcess);
    // The worker inherited the original console policy. Both receive Ctrl-C;
    // keep the supervisor alive to propagate its eventual exit status.
    (void)SetConsoleCtrlHandler(nullptr, TRUE);

    CloseHandle(process.hThread);
    // No kill-on-close Job: collector loss must leave the healthy app alive.
    return n;
}
Result<Native> adopt(std::string_view text, Packet& boot) {
    Native n;
    std::intptr_t* values[]{&n.channel, &n.event, &n.ack, &n.process};
    std::size_t at = 0;
    for (auto* value : values) {
        const auto end = text.find(':', at);
        const auto part =
            text.substr(at, end == std::string_view::npos ? text.size() - at : end - at);
        std::uint64_t parsed = 0;
        if (part.empty() || part.size() > 20) return failure();
        for (char digit : part) {
            if (digit < '0' || digit > '9' ||
                parsed > (UINTPTR_MAX - static_cast<unsigned>(digit - '0')) / 10)
                return failure();
            parsed = parsed * 10 + static_cast<unsigned>(digit - '0');
        }
        *value = static_cast<std::intptr_t>(parsed);
        if (end == std::string_view::npos) {
            if (value != &n.process) return failure();
            at = text.size();
        } else
            at = end + 1;
    }
    if (at != text.size()) return failure();
    n.shared = MapViewOfFile(handle(n.channel), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
    if (!n.shared) return failure();
    const auto& shared = *static_cast<Shared*>(n.shared);
    boot = shared.boot;
    if (!valid(boot) || boot.kind != Message::Boot || boot.pid != GetProcessId(handle(n.process))) {
        UnmapViewOfFile(n.shared);
        return failure();
    }
    for (auto value : {n.channel, n.event, n.ack, n.process})
        SetHandleInformation(handle(value), HANDLE_FLAG_INHERIT, 0);
    return n;
}
bool publish(Native& n, const Packet& packet) noexcept {
    const unsigned index = packet.kind == Message::Ready      ? 0
                           : packet.kind == Message::Expected ? 1
                                                              : 2;
    auto& shared = *static_cast<Shared*>(n.shared);
    shared.messages[index] = packet;
    shared.ready.fetch_or(1U << index, std::memory_order_release);
    return SetEvent(handle(n.event)) != FALSE;
}
bool receive(Native& n, Packet& packet) {
    auto& shared = *static_cast<Shared*>(n.shared);
    for (;;) {
        const auto ready = shared.ready.load(std::memory_order_acquire) & ~n.seen;
        for (unsigned index : {2U, 0U, 1U})
            if (ready & (1U << index)) {
                packet = shared.messages[index];
                n.seen |= 1U << index;
                return valid(packet);
            }
        const HANDLE waits[]{handle(n.event), handle(n.process)};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            const auto final = shared.ready.load(std::memory_order_acquire) & ~n.seen;
            if (!final) return false;
        }
    }
}
void acknowledge(Native& n) noexcept { SetEvent(handle(n.ack)); }
void awaitAcknowledgement(Native& n) noexcept { WaitForSingleObject(handle(n.ack), 1000); }
int wait(Native& n, bool& fatal) {
    WaitForSingleObject(handle(n.process), INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(handle(n.process), &code);
    fatal = (code & 0xc0000000U) == 0xc0000000U;
    return static_cast<int>(code);
}
void close(Native& n) {
    if (n.shared) UnmapViewOfFile(n.shared);
    for (auto value : {n.channel, n.process, n.event, n.ack})
        if (value != -1 && handle(value)) CloseHandle(handle(value));
    n = {};
}
void closeProtocolStdio() {
    _close(0);
    _close(1);
    SetStdHandle(STD_INPUT_HANDLE, nullptr);
    SetStdHandle(STD_OUTPUT_HANDLE, nullptr);
}
}  // namespace supple::diagnostics::detail
