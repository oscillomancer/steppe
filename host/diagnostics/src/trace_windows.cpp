// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "trace.h"
// Windows collector, run by the supervisor inside the worker's terminal hold. The
// supervisor created the worker, so its process handle already carries every right
// needed: nothing is raised and no privilege or policy changes. Threads are
// suspended one at a time, only while their context is read, and resumed before
// the next; every suspended thread is resumed on every exit path. The stack walk is
// the OS unwinder (StackWalk64 over each image's unwind data) with memory read
// through ReadProcessMemory. DbgHelp is process-global state, so all of it runs
// under one lock, with no symbol path, no environment symbol path and no symbol
// server: it loads module images from their local files without symbols, and a
// module's file path never leaves this process. Reports carry the PE CodeView
// GUID and age and module-relative offsets only. x64 only; other Windows
// architectures report the faulting thread as unsupported.
namespace supple::diagnostics::detail {
#if defined(_M_X64) || defined(__x86_64__)
namespace {
// The worker waits one second for its acknowledgement; collection keeps well inside.
constexpr std::chrono::milliseconds kBudget{600};
constexpr std::size_t kMaxListedThreads = 4096;
constexpr std::size_t kMaxQueries = 65536;
constexpr DWORD kExecutable =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

// Evidence for the test fixture's sink (trace.h); formats nothing without one, and stops
// after a fixed number of lines so a long stack cannot flood a log.
template <class... Args>
void note(const char* format, Args... args) {
    static int lines = 0;
    if (!traceEvidenceEnabled() || lines >= 200) return;
    ++lines;
    char line[256];
    std::snprintf(line, sizeof(line), format, args...);
    traceEvidence(line);
}
unsigned long long ull(std::uint64_t value) { return static_cast<unsigned long long>(value); }

std::mutex& dbghelpLock() {
    static std::mutex lock;
    return lock;
}

class Collector;
Collector* active = nullptr;  // the one collector running, guarded by dbghelpLock()

class Collector {
  public:
    explicit Collector(HANDLE process)
        : process_(process),
          pid_(GetProcessId(process)),
          deadline_(std::chrono::steady_clock::now() + kBudget) {
        // Local files only. These options are process-wide; nothing else in the
        // supervisor uses DbgHelp.
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_IGNORE_NT_SYMPATH);
        symbols_ = SymInitializeW(process_, L"", FALSE) != FALSE;
        active = this;
    }
    ~Collector() {
        active = nullptr;
        for (auto it = stopped_.begin(); it != stopped_.end();) {
            const auto id = it->first;
            ++it;
            release(id);
        }
        if (symbols_) SymCleanup(process_);
    }
    Collector(const Collector&) = delete;
    Collector& operator=(const Collector&) = delete;

    bool expired() const { return std::chrono::steady_clock::now() >= deadline_; }
    bool read(std::uint64_t address, void* out, std::size_t bytes) const {
        SIZE_T got = 0;
        return ReadProcessMemory(process_,
                                 reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
                                 out, bytes, &got) != FALSE &&
               got == bytes;
    }
    // Executable committed pages, grouped into modules by image base. A region
    // with no image behind it (generated code) has no module.
    std::optional<Layout> layout() {
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        const auto limit = reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress);
        const ReadMemory reader = [this](std::uint64_t a, void* out, std::size_t n) {
            return read(a, out, n);
        };
        Layout layout;
        std::map<std::uint64_t, int> index;
        std::uint64_t address = 0;
        bool queried = false;
        for (std::size_t queries = 0; address < limit && queries < kMaxQueries; ++queries) {
            if ((queries & 255) == 255 && expired()) break;
            MEMORY_BASIC_INFORMATION page{};
            if (VirtualQueryEx(process_,
                               reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)),
                               &page, sizeof(page)) != sizeof(page))
                break;
            queried = true;
            const auto start = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
            const std::uint64_t end = std::uint64_t{start} + page.RegionSize;
            if (end <= address) break;  // no progress: never loop
            address = end;
            if (page.State != MEM_COMMIT || (page.Protect & PAGE_GUARD) != 0 ||
                (page.Protect & kExecutable) == 0)
                continue;
            if (layout.regions.size() >= kMaxMappings) break;
            Region region{start, end, -1};
            if (page.Type == MEM_IMAGE) {
                const auto base = reinterpret_cast<std::uintptr_t>(page.AllocationBase);
                auto found = index.find(base);
                if (found == index.end() && layout.modules.size() < kMaxModules) {
                    Module module;
                    std::uint64_t bias = base;
                    module.id = peDebugId(reader, base, &bias);
                    module.bias = bias;
                    layout.modules.push_back(std::move(module));
                    bases_.push_back(base);
                    found = index.emplace(base, static_cast<int>(layout.modules.size() - 1)).first;
                }
                if (found != index.end())
                    region.module = found->second;
                else
                    layout.truncatedModules = true;
            }
            layout.regions.push_back(region);
        }
        if (!queried) return std::nullopt;
        return layout;
    }
    std::vector<std::uint64_t> threads() const {
        std::vector<std::uint64_t> tids;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return tids;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more;
             more = Thread32Next(snapshot, &entry))
            if (entry.th32OwnerProcessID == pid_ && tids.size() < kMaxListedThreads)
                tids.push_back(entry.th32ThreadID);
        CloseHandle(snapshot);
        return tids;
    }
    // The main thread is the process's first: the earliest creation time.
    std::uint64_t mainThread(const std::vector<std::uint64_t>& tids) const {
        std::uint64_t first = 0, earliest = ~std::uint64_t{0};
        for (const auto id : tids) {
            const HANDLE thread =
                OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(id));
            if (!thread) continue;
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                const std::uint64_t when =
                    (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
                if (when < earliest) {
                    earliest = when;
                    first = id;
                }
            }
            CloseHandle(thread);
        }
        return first;
    }
    Missing stop(std::uint64_t id, Regs& regs) {
        const HANDLE thread = OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
            static_cast<DWORD>(id));
        if (!thread)
            return GetLastError() == ERROR_INVALID_PARAMETER ? Missing::ThreadExited
                                                             : Missing::AccessDenied;
        if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
            const DWORD error = GetLastError();
            DWORD code = STILL_ACTIVE;
            const bool gone = GetExitCodeThread(thread, &code) && code != STILL_ACTIVE;
            CloseHandle(thread);
            return gone || error == ERROR_INVALID_PARAMETER ? Missing::ThreadExited
                                                            : Missing::AccessDenied;
        }
        Stopped& entry = stopped_[id];
        entry.thread = thread;
        entry.context = CONTEXT{};
        entry.context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (!GetThreadContext(thread, &entry.context)) {
            release(id);
            return Missing::RegistersUnreadable;
        }
        note("evidence: thread stopped rip=%llx rsp=%llx rbp=%llx\n", ull(entry.context.Rip),
             ull(entry.context.Rsp), ull(entry.context.Rbp));
        regs = {entry.context.Rip, entry.context.Rsp, entry.context.Rbp};
        return Missing::None;
    }
    void release(std::uint64_t id) {
        const auto found = stopped_.find(id);
        if (found == stopped_.end()) return;
        (void)ResumeThread(found->second.thread);
        CloseHandle(found->second.thread);
        stopped_.erase(found);
    }
    // Walks one thread. A suspended thread has its whole context; the faulting
    // thread, parked in its filter, only the pc, stack and frame pointers from the
    // capsule, which is what the unwinder needs for most frames.
    Walk walk(std::uint64_t id, const Regs& regs, const Layout& layout) {
        Walk result;
        layout_ = &layout;
        CONTEXT context{};
        const auto stopped = stopped_.find(id);
        if (stopped != stopped_.end()) {
            context = stopped->second.context;
        } else {
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            context.Rip = regs.pc;
            context.Rsp = regs.sp;
            context.Rbp = regs.fp;
        }
        STACKFRAME64 frame{};
        frame.AddrPC = {context.Rip, 0, AddrModeFlat};
        frame.AddrFrame = {context.Rbp, 0, AddrModeFlat};
        frame.AddrStack = {context.Rsp, 0, AddrModeFlat};
        // The stopped instruction is always recorded; the unwinder's first report is
        // that same frame, so it only sets the stack mark.
        if (auto start = layoutFrame(layout, context.Rip, false))
            result.frames.push_back(*start);
        else
            result.frames.push_back(Frame{});  // pc outside executable memory: nothing to record
        std::uint64_t previousStack = 0;
        bool initial = true;
        for (;;) {
            if (expired()) {
                result.stop = Stop::Deadline;
                break;
            }
            // The thread handle is not needed: memory and unwind data come from the
            // callbacks.
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process_, nullptr, &frame, &context,
                             &readThunk, &tableThunk, &baseThunk, nullptr)) {
                note("evidence: StackWalk64 ended after %zu frames, error %lu\n",
                     result.frames.size(), static_cast<unsigned long>(GetLastError()));
                result.stop = Stop::End;
                break;
            }
            const std::uint64_t pc = frame.AddrPC.Offset;
            note("evidence: step pc=%llx sp=%llx initial=%d executable=%d\n", ull(pc),
                 ull(frame.AddrStack.Offset), initial ? 1 : 0,
                 layoutFrame(layout, pc, true) ? 1 : 0);
            if (pc == 0) {
                result.stop = Stop::End;
                break;
            }
            if (initial) {
                initial = false;
                previousStack = frame.AddrStack.Offset;
                continue;
            }
            if (frame.AddrStack.Offset <= previousStack) {
                result.stop = Stop::Cycle;
                break;
            }
            previousStack = frame.AddrStack.Offset;
            if (result.frames.size() >= kMaxFrames) {
                result.stop = Stop::Depth;
                break;
            }
            const auto found = layoutFrame(layout, pc, true);
            if (!found) {
                result.stop = Stop::NotExecutable;
                break;
            }
            result.frames.push_back(*found);
        }
        layout_ = nullptr;
        return result;
    }

  private:
    struct Stopped {
        HANDLE thread = nullptr;
        CONTEXT context{};
    };
    // DbgHelp wants image base addresses it has loaded. Load a module the first time
    // a walk needs it, from its local file, without symbols.
    DWORD64 baseOf(DWORD64 address) {
        if (!symbols_ || !layout_) return 0;
        const auto region =
            std::upper_bound(layout_->regions.begin(), layout_->regions.end(), address,
                             [](std::uint64_t a, const Region& r) { return a < r.start; });
        if (region == layout_->regions.begin()) return 0;
        const Region& found = *(region - 1);
        if (address >= found.end || found.module < 0 ||
            static_cast<std::size_t>(found.module) >= bases_.size())
            return 0;
        const std::uint64_t base = bases_[static_cast<std::size_t>(found.module)];
        auto loaded = loaded_.find(base);
        if (loaded == loaded_.end()) {
            // Reading a module's file can be slow (a cold disk, a virus scan, a share):
            // new modules load only while the hold's budget remains. A single load
            // cannot be interrupted, so the bound is "no new load after the deadline".
            if (expired()) {
                note("evidence: module at %llx not loaded, hold budget spent\n", ull(base));
                return 0;
            }
            std::wstring path(32768, L'\0');
            const DWORD length = GetModuleFileNameExW(
                process_, reinterpret_cast<HMODULE>(static_cast<std::uintptr_t>(base)), path.data(),
                static_cast<DWORD>(path.size()));
            bool ok = false;
            if (length > 0 && length < path.size()) {
                path.resize(length);
                SetLastError(ERROR_SUCCESS);
                ok = SymLoadModuleExW(process_, nullptr, path.c_str(), nullptr, base, 0, nullptr,
                                      SLMFLAG_NO_SYMBOLS) != 0 ||
                     GetLastError() == ERROR_SUCCESS;  // zero with no error: already loaded
            }
            note("evidence: module at %llx %s (name length %lu, error %lu)\n", ull(base),
                 ok ? "loaded for unwinding" : "NOT LOADED", static_cast<unsigned long>(length),
                 static_cast<unsigned long>(GetLastError()));
            loaded = loaded_.emplace(base, ok).first;
        }
        return loaded->second ? base : 0;
    }
    static BOOL CALLBACK readThunk(HANDLE, DWORD64 address, PVOID buffer, DWORD size, LPDWORD got) {
        SIZE_T count = 0;
        const bool ok = active && active->read(address, buffer, size);
        if (ok) count = size;
        if (got) *got = static_cast<DWORD>(count);
        return ok ? TRUE : FALSE;
    }
    static PVOID CALLBACK tableThunk(HANDLE process, DWORD64 address) {
        if (!active || active->baseOf(address) == 0) {
            note("evidence: no module for unwind data at %llx\n", ull(address));
            return nullptr;
        }
        PVOID table = SymFunctionTableAccess64(process, address);
        if (!table) note("evidence: no function table at %llx\n", ull(address));
        return table;
    }
    static DWORD64 CALLBACK baseThunk(HANDLE, DWORD64 address) {
        return active ? active->baseOf(address) : 0;
    }

    HANDLE process_;
    DWORD pid_;
    std::chrono::steady_clock::time_point deadline_;
    bool symbols_ = false;
    std::vector<std::uint64_t> bases_;  // image base of each layout module, by index
    std::map<std::uint64_t, bool> loaded_;
    const Layout* layout_ = nullptr;
    std::map<std::uint64_t, Stopped> stopped_;
};
}  // namespace
TraceSnapshot collectProcessTrace(Native& native, const Packet& terminal) {
    std::lock_guard<std::mutex> serialized(dbghelpLock());
    Collector collector(reinterpret_cast<HANDLE>(native.process));
    const auto tids = collector.threads();
    TraceOps ops;
    ops.expired = [&] { return collector.expired(); };
    ops.read = [&](std::uint64_t a, void* out, std::size_t n) { return collector.read(a, out, n); };
    ops.layout = [&] { return collector.layout(); };
    ops.threads = [&] { return tids; };
    ops.stop = [&](std::uint64_t tid, Regs& regs) { return collector.stop(tid, regs); };
    ops.release = [&](std::uint64_t tid) { collector.release(tid); };
    ops.walk = [&](std::uint64_t tid, const Regs& regs, const Layout& layout) {
        return collector.walk(tid, regs, layout);
    };
    return collectTrace(ops, terminal.tid, Regs{terminal.pc, terminal.sp, terminal.frame},
                        collector.mainThread(tids), hostArch(), native.table);
}
#else
TraceSnapshot collectProcessTrace(Native&, const Packet&) {
    TraceSnapshot snapshot;
    ThreadTrace fault;
    fault.role = Role::Fault;
    fault.missing = Missing::Unsupported;
    snapshot.threads.push_back(fault);
    return snapshot;
}
#endif
}  // namespace supple::diagnostics::detail
