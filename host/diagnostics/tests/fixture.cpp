// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/cmd/bus.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include "../src/terminal.h"
#include "../src/trace.h"
#include "../src/watch.h"
#include "supple/base/rt.h"
#include "supple/diagctx/inventory.h"
#include "supple/diagctx/scope.h"
#include "supple/diagctx/table.h"
#include "supple/diagnostics/frontend.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <chrono>
#include <cstring>
#include <fstream>
#else
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <csignal>
#if defined(__linux__)
#include <link.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>
#endif
#endif
namespace {
std::atomic<std::uintptr_t> badAddress{0};
void actualFault() SUPPLE_RT { *reinterpret_cast<volatile int*>(badAddress.load()) = 1; }
std::atomic<unsigned> depth{0};
#if defined(__clang__)
[[clang::optnone]]
#elif defined(__GNUC__)
[[gnu::optimize("O0")]]
#endif
[[gnu::noinline]] unsigned exhaustStack() {
    volatile char data[8192];
    data[0] = 1;
    const unsigned next = depth.fetch_add(1);
    return next < 1000000 ? exhaustStack() + static_cast<unsigned>(data[0]) : 0;
}
#if defined(__linux__)
// Trace fixtures. Each marked function sits on its own page so a return address
// maps to exactly one of them, whatever the optimization level.
std::atomic<unsigned> parked{0};
std::atomic<bool> neverReleased{false};
[[gnu::noinline, gnu::aligned(4096)]] void parkInner() {
    while (!neverReleased.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
[[gnu::noinline, gnu::aligned(4096)]] void parkOuter() {
    parked.fetch_add(1);
    parkInner();
    asm volatile("" ::: "memory");
}
[[gnu::noinline, gnu::aligned(4096)]] void faultMiddle() {
    actualFault();
    asm volatile("" ::: "memory");
}
[[gnu::noinline, gnu::aligned(4096)]] void faultOuter() {
    faultMiddle();
    asm volatile("" ::: "memory");
}
// The executable's load bias and GNU build ID, found by reading its own program
// headers, independently of the collector's parser.
int imageInfo(dl_phdr_info* info, std::size_t, void* out) {
    auto* result = static_cast<std::pair<std::uintptr_t, std::string>*>(out);
    result->first = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto* at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        std::size_t pos = 0;
        while (pos + 12 <= ph.p_filesz) {
            std::uint32_t namesz, descsz, kind;
            std::memcpy(&namesz, at + pos, 4);
            std::memcpy(&descsz, at + pos + 4, 4);
            std::memcpy(&kind, at + pos + 8, 4);
            const std::size_t desc = pos + 12 + ((namesz + 3) & ~3U);
            if (kind == 3) {
                for (std::uint32_t b = 0; b < descsz; ++b) {
                    result->second += "0123456789abcdef"[at[desc + b] >> 4];
                    result->second += "0123456789abcdef"[at[desc + b] & 15];
                }
                return 1;
            }
            pos = desc + ((descsz + 3) & ~3U);
        }
    }
    return 1;  // the first object is the executable
}
void writeExpected(const char* path) {
    std::pair<std::uintptr_t, std::string> image;
    dl_iterate_phdr(imageInfo, &image);
    auto offset = [&](auto function) {
        return reinterpret_cast<std::uintptr_t>(function) - image.first;
    };
    std::ofstream out(path);
    out << "{\"build_id\":\"" << image.second << "\",\"parkOuter\":" << offset(&parkOuter)
        << ",\"faultMiddle\":" << offset(&faultMiddle) << ",\"faultOuter\":" << offset(&faultOuter)
        << ",\"pid\":" << ::getpid() << "}\n";
}
#endif
#if defined(_WIN32)
// Trace fixtures, as on Linux but without page alignment: the lifecycle test
// attributes a frame to the nearest marked function start below it.
std::atomic<unsigned> parked{0};
std::atomic<bool> neverReleased{false};
std::atomic<unsigned> sink{0};  // work after each call keeps it from being a tail call
[[gnu::noinline]] void parkInner() {
    while (!neverReleased.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    sink.fetch_add(1);
}
[[gnu::noinline]] void parkOuter() {
    parked.fetch_add(1);
    parkInner();
    sink.fetch_add(1);
}
[[gnu::noinline]] void faultMiddle() {
    actualFault();
    sink.fetch_add(1);
}
[[gnu::noinline]] void faultOuter() {
    faultMiddle();
    sink.fetch_add(1);
}
// The executable's CodeView identity and load bias, read from its own headers with the
// SDK's structures, independently of the collector's parser. The id is the PDB GUID in
// symbol-server order followed by the age, 40 lowercase hex characters; empty when the
// image has no CodeView record.
void writeExpected(const char* path) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::uintptr_t bias = base - static_cast<std::uintptr_t>(nt->OptionalHeader.ImageBase);
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    std::string id;
    const auto* entries =
        reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + directory.VirtualAddress);
    for (DWORD i = 0;
         directory.VirtualAddress && i < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        const auto* record =
            reinterpret_cast<const unsigned char*>(base + entries[i].AddressOfRawData);
        if (std::memcmp(record, "RSDS", 4) != 0) continue;
        GUID guid;
        DWORD age;
        std::memcpy(&guid, record + 4, sizeof(guid));
        std::memcpy(&age, record + 20, sizeof(age));
        char text[64];
        std::snprintf(text, sizeof(text), "%08lx%04x%04x", static_cast<unsigned long>(guid.Data1),
                      static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3));
        id = text;
        for (unsigned char byte : guid.Data4) {
            std::snprintf(text, sizeof(text), "%02x", static_cast<unsigned>(byte));
            id += text;
        }
        std::snprintf(text, sizeof(text), "%08lx", static_cast<unsigned long>(age));
        id += text;
        break;
    }
    auto offset = [&](auto function) { return reinterpret_cast<std::uintptr_t>(function) - bias; };
    std::ofstream out(path);
    out << "{\"build_id\":\"" << id << "\",\"parkOuter\":" << offset(&parkOuter)
        << ",\"faultMiddle\":" << offset(&faultMiddle) << ",\"faultOuter\":" << offset(&faultOuter)
        << ",\"pid\":" << GetCurrentProcessId() << "}\n";
}
// An earlier handler that deals with the exception, so it never reaches the
// last-chance filter. A vectored handler, not __try: clang-cl rejects the
// language extension under -Werror.
std::atomic<bool> expectFault{false};
LONG CALLBACK priorHandler(EXCEPTION_POINTERS* info) {
    if (expectFault.load() && info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
int handledFault() {
    void* const handle = AddVectoredExceptionHandler(1, priorHandler);
    if (!handle) return 9;
    expectFault = true;
    RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);  // continuable
    expectFault = false;
    RemoveVectoredExceptionHandler(handle);
    return 0;
}
#else
volatile sig_atomic_t handled = 0;
void priorSignal(int) { handled = 1; }
#endif
void priorTerminate() {
    std::fputs("prior terminate handler\n", stderr);
    std::_Exit(44);
}
#if defined(_WIN32)
// A top-level filter that was already in place when the worker installed its own, as the
// MSVC runtime's always is. The last-chance filter must chain to it after its handoff.
bool filterResumes = false;
LONG WINAPI priorFilter(EXCEPTION_POINTERS*) {
    std::fputs("prior filter ran\n", stderr);
    return filterResumes ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
}
#endif
// The supervisor watch (ADR-0091). The program's stop is a SIGTERM handler that sets a flag,
// as the model loop's does; the body reports what the watcher concluded, in lines a test
// can read, and then either takes the stop or carries on.
// A thread whose body throws out of it: the exception reaches std::terminate on that thread.
// A std::thread's entry and a raw one (CreateThread, pthread_create) differ in how the
// runtime gets there, and the terminate handler is per thread under the MSVC runtime, so
// both are tried, with and without the thread having registered (review round 35, F1).
void throwOut(bool registered) {
    if (registered) (void)supple::diagnostics::registerCurrentThread();
    throw std::runtime_error("thread body");
}
#if defined(_WIN32)
DWORD WINAPI rawThreadMain(LPVOID argument) {
    throwOut(argument != nullptr);
    return 0;
}
void runRawThread(bool registered) {
    HANDLE thread =
        CreateThread(nullptr, 0, rawThreadMain, registered ? &registered : nullptr, 0, nullptr);
    if (thread) WaitForSingleObject(thread, INFINITE);
}
#else
void* rawThreadMain(void* argument) {
    throwOut(argument != nullptr);
    return nullptr;
}
void runRawThread(bool registered) {
    pthread_t thread;
    if (::pthread_create(&thread, nullptr, rawThreadMain, registered ? &registered : nullptr) == 0)
        ::pthread_join(thread, nullptr);
}
#endif
std::atomic<bool> stopRequested{false};
void onStop(int) { stopRequested.store(true); }
int watchBody(const std::string& mode, int channel = -1) {
    namespace detail = supple::diagnostics::detail;
    std::signal(SIGTERM, onStop);
#if defined(_WIN32)
    std::cout << GetCurrentProcessId() << std::endl;
#else
    std::cout << ::getpid() << std::endl;
#endif
    detail::SupervisorEnd end;
    while ((end = detail::supervisorEnd()) == detail::SupervisorEnd::Pending)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::cout << "decision " << detail::supervisorEndName(end) << std::endl;
    if (end == detail::SupervisorEnd::Launcher) {
        while (!stopRequested.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::cout << "stop requested" << std::endl;
        // A stop that never finishes: only the bound ends this process.
        if (mode == "supervisor-killed-hung")
            for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << "stopped" << std::endl;
        return 0;
    }
#if !defined(_WIN32)
    // On a channel the test holds (watch-direct): the watcher must have left the first byte
    // where it was, for the terminal handler's wait to see.
    char pending = 0;
    if (channel >= 0 && ::recv(channel, &pending, 1, MSG_DONTWAIT) == 1) {
        char text[16];
        std::snprintf(text, sizeof(text), "pending %02x", static_cast<unsigned char>(pending));
        std::cout << text << std::endl;
    }
#else
    (void)channel;
#endif
    std::string line;
    std::getline(std::cin, line);
    if (mode == "fault-after-loss") actualFault();
    std::cout << line << std::endl;
    return 0;
}
// Crashes the supervisor once the worker's Ready packet is in: a fault, or an abort.
void crashSupervisorByFault(const supple::diagnostics::detail::Packet& packet) {
    if (packet.kind == supple::diagnostics::detail::Message::Ready) actualFault();
}
void crashSupervisorByStack(const supple::diagnostics::detail::Packet& packet) {
    if (packet.kind == supple::diagnostics::detail::Message::Ready) (void)exhaustStack();
}
void crashSupervisorByAbort(const supple::diagnostics::detail::Packet& packet) {
    if (packet.kind == supple::diagnostics::detail::Message::Ready) std::abort();
}
int body(int argc, char** argv) {
    if (argc < 3) return 2;
    const std::string mode = argv[2];
    if (mode == "normal") return 7;
    if (mode == "negative-return") return -1;
    if (mode == "exit") std::_Exit(31);
#if !defined(_WIN32)
    if (mode == "fork-channel") {
        const auto helper = ::fork();
        if (helper < 0) return 2;
        if (helper == 0) {
            // Keep the diagnostic channel, but no protocol output pipe writer.
            ::close(STDOUT_FILENO);
            ::close(STDERR_FILENO);
            char release;
            while (::read(STDIN_FILENO, &release, 1) < 0 && errno == EINTR) {
            }
            ::_exit(0);
        }
        std::cout << helper << std::endl;
        std::_Exit(31);
    }
#endif
    if (mode == "abort") std::abort();
    if (mode == "terminate") std::terminate();
    if (mode == "prior-terminate") {
        std::terminate();
    }
    // An exception nobody catches reaches the terminate handler on every platform, and so
    // the program's own handler (on Windows it first meets the top-level filter).
    if (mode == "prior-terminate-throw") throw std::runtime_error("uncaught");
#if defined(_WIN32)
    // Evidence for a failed capture: whether the last-chance filter is in place.
    if (mode == "fault" || mode == "prior-filter" || mode == "prior-filter-resume")
        std::fprintf(stderr, "last-chance filter installed: %s\n",
                     supple::diagnostics::detail::lastChanceFilterInstalled() ? "yes" : "no");
    // A continuable exception whose chained filter asks to resume: the fault must stay
    // fatal, so the code after it must never run.
    if (mode == "prior-filter-resume") {
        RaiseException(0xE0000001, 0, 0, nullptr);
        std::fputs("resumed after the fault\n", stderr);
        return 0;
    }
#endif
    if (mode == "fault" || mode == "prior-filter") actualFault();
    if (mode == "thread-fault") {
        std::thread worker([] {
            (void)supple::diagnostics::registerCurrentThread();
            actualFault();
        });
        worker.join();
    }
    if (mode == "thread-throw" || mode == "thread-throw-registered") {
        std::thread thrower(
            [registered = mode == "thread-throw-registered"] { throwOut(registered); });
        thrower.join();
    }
    if (mode == "raw-thread-throw") runRawThread(false);
    if (mode == "raw-thread-throw-registered") runRawThread(true);
#if defined(_WIN32)
    // The code 0xE06D7363 raised by hand, which the prior filter does not turn into
    // std::terminate: nobody handles it, so it is a fault like any other (review round 35, F2).
    if (mode == "prior-filter-cpp-code")
        RaiseException(0xE06D7363, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
    // Command breadcrumbs (topic 5d), through a real registry and dispatcher: a few commands
    // run from different origins, then one faults inside its handler.
    //   commands-real   the faulting command is called directly
    //   commands-batch  it is the second step of a batch
    //   commands-other  it is run from a helper thread (not the model thread): no entry for it
    if (mode.rfind("commands-", 0) == 0) {
        namespace cmd = supple::cmd;
        struct Count {
            int count = 0;
        };
        struct Host : cmd::Host {
            cmd::Context makeContext(const cmd::Origin& origin) override {
                return cmd::Context(origin);
            }
            supple::Result<std::uint64_t> beginTransaction(const cmd::Origin&,
                                                           std::string_view) override {
                return std::uint64_t{1};
            }
            supple::Status commitTransaction(std::uint64_t) override { return {}; }
            supple::Status rollbackTransaction(std::uint64_t) override { return {}; }
            void log(const cmd::LogRecord&) override {}
            void emit(std::string_view, supple::json::Value) override {}
        };
        const auto count = supple::schema::object<Count>("count")
                               .field("count", &Count::count, supple::schema::integerOf<int>())
                               .build();
        cmd::Registry registry;
        for (const char* name : {"fixture.fine", "fixture.note-secret", "fixture.crash"}) {
            cmd::Command<cmd::NoArgs, Count> command;
            command.name = name;
            command.kind = cmd::Kind::Query;
            command.exposure = cmd::Exposure::Internal;
            command.summary = "A fixture command.";
            command.args = supple::schema::object<cmd::NoArgs>("no_args").build();
            command.result = count;
            const std::string_view which = name;
            command.run = [which](const cmd::NoArgs&, cmd::Context&) -> supple::Result<Count> {
                if (which == "fixture.crash") actualFault();
                return Count{1};
            };
            registry.add(std::move(command));
        }
        Host host;
        cmd::Dispatcher dispatcher(registry, host);
        const auto run = [&dispatcher](const char* name, cmd::Origin::Type type) {
            (void)dispatcher.execute({name, supple::json::Value::object(),
                                      cmd::Origin{type, "client-canary", "agent-canary"}});
        };
        run("fixture.fine", cmd::Origin::Type::Ui);
        run("fixture.no-such-command", cmd::Origin::Type::Mcp);  // unknown: never recorded
        run("fixture.note-secret", cmd::Origin::Type::Lua);
        if (mode == "commands-real") run("fixture.crash", cmd::Origin::Type::Agent);
        if (mode == "commands-batch") {
            auto steps = supple::json::Value::array();
            for (const char* name : {"fixture.fine", "fixture.crash"}) {
                auto step = supple::json::Value::object();
                step["command"] = name;
                step["args"] = supple::json::Value::object();
                steps.push_back(std::move(step));
            }
            auto args = supple::json::Value::object();
            args["label"] = "canary label";
            args["steps"] = std::move(steps);
            (void)dispatcher.execute(
                {"batch", std::move(args), cmd::Origin{cmd::Origin::Type::Rpc, "", ""}});
        }
        if (mode == "commands-other") {
            // The crash belongs to a thread that does not own the ring: the command is not
            // listed, and the report says commands were refused.
            std::thread helper([&] {
                (void)supple::diagnostics::registerCurrentThread();
                run("fixture.crash", cmd::Origin::Type::Mcp);
            });
            helper.join();
        }
    }
    // The plug-in inventory (topic 5c). The host announces its plug-ins as it creates them; the
    // fixture does the same by hand with strings that must reach the report only bounded and
    // clean, then faults while a thread is inside an entry of one of them.
    //   inventory-process      a registered helper thread faults inside `process`
    //   inventory-state        the main thread faults inside `state`
    //   inventory-constructor  the main thread faults inside `create`, the instance still loading
    //   inventory-other        the main thread faults inside `params` while a helper is inside
    //                          `process` of the same instance
    if (mode.rfind("inventory-", 0) == 0) {
        namespace dc = supple::diagctx;
        const auto image = reinterpret_cast<std::uint64_t>(&actualFault);
        const auto describe = [image](dc::InstanceState state, std::uint32_t cell,
                                      std::string_view name) {
            dc::announce(cell, dc::InstanceKind::Clap, dc::InstanceOrigin::Foreign,
                         dc::InstanceMode::InProcess, state, image,
                         {name, std::string(100, 'V'), "1.0\x01\x7f", "com.canary.reverb"});
        };
        // An instance that came and went before the fault: it is listed as destroyed, and its
        // cell, handed out again below, is what the next one's description replaces.
        const auto gone = dc::acquireCell();
        describe(dc::InstanceState::Active, gone, "Retired Canary");
        dc::announceRemove(gone);
        dc::releaseCell(gone);
        const auto cell = dc::acquireCell();
        describe(dc::InstanceState::Loading, cell, "Canary Reverb\x1b[31m");
        if (mode == "inventory-constructor") {
            dc::MainScope scope(cell, dc::Entry::Create);
            actualFault();
        }
        dc::announceState(cell, dc::InstanceState::Inactive);
        dc::announceState(cell, dc::InstanceState::Active);
        if (mode == "inventory-state") {
            dc::MainScope scope(cell, dc::Entry::State);
            actualFault();
        }
        if (mode == "inventory-process") {
            std::thread worker([cell] {
                (void)supple::diagctx::registerThread(supple::diagctx::ThreadRole::AudioRender);
                (void)supple::diagnostics::registerCurrentThread();
                supple::diagctx::AudioScope scope(cell, supple::diagctx::Entry::Process);
                actualFault();
            });
            worker.join();
        }
        if (mode == "inventory-other") {
            static std::atomic<bool> inside{false};
            std::thread helper([cell] {
                (void)supple::diagctx::registerThread(supple::diagctx::ThreadRole::AudioLive, 1);
                supple::diagctx::AudioScope scope(cell, supple::diagctx::Entry::Process);
                inside.store(true);
                for (;;) supple::rt::relax();
            });
            helper.detach();
            while (!inside.load()) supple::rt::relax();
            dc::MainScope scope(cell, dc::Entry::Params);
            actualFault();
        }
    }
#if defined(__linux__) || defined(_WIN32)
    if (mode == "trace" || mode == "trace-nondumpable") {
        if (argc != 4) return 4;
        writeExpected(argv[3]);
        const bool nondumpable = mode == "trace-nondumpable";  // Linux only
        // Owned threads register their roles, as the engine's do (topic 5): the report
        // must label each by what it registered as, and the faulting one as the fault.
        using supple::diagctx::ThreadRole;
        std::thread first([] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioLive, 2);
            parkOuter();
        });
        std::thread second([] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioBackground, 1);
            parkOuter();
        });
        std::thread faulting([nondumpable] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioRender);
            (void)supple::diagnostics::registerCurrentThread();
            while (parked.load() != 3) supple::rt::relax();  // two threads plus main
#if defined(__linux__)
            if (nondumpable) (void)::prctl(PR_SET_DUMPABLE, 0);
#else
            (void)nondumpable;
#endif
            faultOuter();
        });
        parkOuter();  // the main thread parks too; the fault ends the process
        faulting.join();
        first.join();
        second.join();
    }
#endif
#if defined(__linux__) || defined(_WIN32)
    // Execution scopes (topic 5b): every thread of the trace fixture is inside a different
    // entry of some instance when the fault comes, as plug-in code would be, and the report
    // must name the entry each one is in. One thread is not registered, as the audio
    // driver's callback thread is, and is found by the stack it stands on; one is inside two
    // nested scopes and is reported in the inner. The cells are claimed here, cold, as
    // an instance's would be.
    if (mode == "trace-scopes") {
        if (argc != 4) return 4;
        writeExpected(argv[3]);
        using supple::diagctx::acquireCell;
        using supple::diagctx::AudioScope;
        using supple::diagctx::Entry;
        using supple::diagctx::MainScope;
        using supple::diagctx::ThreadRole;
        const auto cellFault = acquireCell(), cellModel = acquireCell(), cellLive = acquireCell(),
                   cellOuter = acquireCell(), cellInner = acquireCell(), cellOther = acquireCell();
        std::thread live([=] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioLive, 2);
            AudioScope scope(cellLive, Entry::Reset);
            parkOuter();
        });
        // Nested scopes belong to different frames, as the host's do (a scope in create(),
        // another in the library open it calls): one frame's scopes share an address.
        static const auto parkInnerScope = [](std::uint32_t cell) {
            struct Frame {
                [[gnu::noinline]] static void run(std::uint32_t cell) {
                    MainScope inner(cell, Entry::LibraryOpen);
                    parkOuter();
                    asm volatile("" ::: "memory");
                }
            };
            Frame::run(cell);
        };
        std::thread background([=] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioBackground, 1);
            MainScope outer(cellOuter, Entry::State);
            parkInnerScope(cellInner);
        });
        std::thread unregistered([=] {
            MainScope scope(cellOther, Entry::Gui);
            parkOuter();
        });
        std::thread faulting([=] {
            (void)supple::diagctx::registerThread(ThreadRole::AudioRender);
            (void)supple::diagnostics::registerCurrentThread();
            while (parked.load() != 4) supple::rt::relax();  // three threads plus main
            AudioScope scope(cellFault, Entry::Process);
            faultOuter();
        });
        {
            MainScope scope(cellModel, Entry::Params);
            parkOuter();  // the main thread parks too; the fault ends the process
        }
        faulting.join();
        live.join();
        background.join();
        unregistered.join();
    }
#endif
#if defined(__linux__)
    // A thread in a killable kernel wait cannot reach a ptrace stop, so the collector
    // gives up on it at its deadline and the supervisor must still see the worker
    // exit and write the report (review round 27, F1). The wait is a vfork until its
    // child execs or exits, as posix_spawn does; the child exits when the worker dies.
    if (mode == "trace-killable") {
        int gate[2];
        if (::pipe(gate) != 0) return 2;
        static std::atomic<long> blockedTid{0};
        std::thread blocked([gate] {
            blockedTid.store(::syscall(SYS_gettid));
            const pid_t child = ::vfork();
            if (child == 0) {
                ::syscall(SYS_close, gate[1]);
                char c;
                ::syscall(SYS_read, gate[0], &c, 1);  // end of file once the worker is gone
                ::syscall(SYS_exit_group, 0);
            }
        });
        while (blockedTid.load() == 0) supple::rt::relax();
        // State, not time: wait until the thread sits in the vfork wait ('D').
        const std::string stat = "/proc/self/task/" + std::to_string(blockedTid.load()) + "/stat";
        for (;;) {
            std::ifstream in(stat);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            const auto close = text.rfind(')');
            if (close != std::string::npos && close + 2 < text.size() && text[close + 2] == 'D')
                break;
            supple::rt::relax();
        }
        std::thread faulting([] {
            (void)supple::diagnostics::registerCurrentThread();
            faultOuter();
        });
        faulting.join();
        blocked.join();
    }
#endif
    if (mode == "stack") return static_cast<int>(exhaustStack());
    if (mode == "concurrent") {
        std::atomic<unsigned> ready{0};
        std::atomic<bool> go{false};
        auto fault = [&] {
            (void)supple::diagnostics::registerCurrentThread();
            ++ready;
            while (!go.load()) supple::rt::relax();
            actualFault();
        };
        std::thread a(fault), b(fault);
        while (ready.load() != 2) supple::rt::relax();
        go = true;
        a.join();
        b.join();
    }
    if (mode == "handled") {
#if defined(_WIN32)
        return handledFault();
#else
        ::raise(SIGSEGV);
        return handled == 1 ? 0 : 9;
#endif
    }
    if (mode == "killed") {
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), 9);
#else
        ::kill(::getpid(), SIGKILL);
#endif
    }
#if defined(_WIN32)
    if (mode == "failfast") RaiseFailFastException(nullptr, nullptr, 0);
#endif
    if (mode == "teardown") {
        std::atexit([] {
#if !defined(_WIN32)
            // Evidence for a teardown abort that left no capsule (native CI on macOS).
            // abort() rewrites the signal mask itself, so the mask is not worth printing;
            // what can make the kernel force a SIGILL instead of running the handler is
            // an alternate stack that is still enabled but no longer mapped (review
            // round 29). Print whether SIGABRT has our handler, and the alternate stack
            // as sigaltstack(2) reports it now: its flags, size, and whether its first
            // page is still mapped (msync fails with ENOMEM on an unmapped page).
            struct sigaction current{};
            (void)::sigaction(SIGABRT, nullptr, &current);
            const bool ours =
                (current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction != nullptr;
            std::fprintf(stderr, "teardown: SIGABRT handler %s\n", ours ? "installed" : "absent");
            stack_t alt{};
            if (::sigaltstack(nullptr, &alt) != 0) {
                std::fprintf(stderr, "teardown: sigaltstack unreadable (errno %d)\n", errno);
            } else if ((alt.ss_flags & SS_DISABLE) != 0) {
                std::fprintf(stderr, "teardown: alternate stack disabled (flags %d)\n",
                             alt.ss_flags);
            } else {
                const bool mapped = ::msync(alt.ss_sp, 1, MS_ASYNC) == 0 || errno != ENOMEM;
                std::fprintf(stderr, "teardown: alternate stack enabled (flags %d, size %zu), %s\n",
                             alt.ss_flags, static_cast<std::size_t>(alt.ss_size),
                             mapped ? "mapped" : "NOT MAPPED");
            }
            std::fflush(stderr);
#endif
            std::abort();
        });
        return 0;
    }
    // The product passes stdin and stdout through byte for byte, and these modes are how a
    // test shows it, so they read and write in binary: the Windows C runtime's text mode
    // would turn every "\n" written into "\r\n" and say nothing about the supervisor.
    const bool watching = mode == "collector-loss" || mode == "collector-loss-abort" ||
                          mode == "collector-loss-stack" || mode == "fault-after-loss" ||
                          mode == "supervisor-killed" || mode == "supervisor-killed-hung";
    if (mode == "echo" || watching) {
#if defined(_WIN32)
        (void)_setmode(_fileno(stdin), _O_BINARY);
        (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    }
    if (mode == "echo") {
        if (argc != 4 || std::string(argv[3]) != "argument with spaces \" and slash\\") return 4;
        std::cout << std::cin.rdbuf();
        return 0;
    }
    if (watching) return watchBody(mode);
    return 2;
}
}  // namespace
int main(int argc, char** argv) {
#if defined(_WIN32)
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
    rlimit limit{0, 0};
    (void)::setrlimit(RLIMIT_CORE, &limit);
#endif
    if (argc < 3) return 2;
#if !defined(_WIN32)
    if (std::string_view(argv[2]) == "terminal-no-ack" && argc == 4) {
        supple::diagnostics::detail::Native native{std::atoi(argv[3])};
        supple::diagnostics::detail::Packet boot;
        boot.role = 3;
        std::fill(boot.incident.begin(), boot.incident.begin() + 32, '0');
        (void)supple::diagnostics::detail::installTerminal(native, boot);
        std::cout << "ready" << std::endl;
        std::string line;
        std::getline(std::cin, line);
        actualFault();
        return 9;
    }
#endif
#if !defined(_WIN32)
    // The watcher on a channel the test holds the other end of: it must call end of file
    // the launcher's stop, and anything else (the notice, or a byte nobody sends) not.
    if (std::string_view(argv[2]) == "watch-direct" && argc == 4) {
        supple::diagnostics::detail::Native native{std::atoi(argv[3])};
        supple::diagnostics::detail::startSupervisorWatch(native);
        std::cout << "ready" << std::endl;
        return watchBody("watch-direct", std::atoi(argv[3]));
    }
#endif
    {
        const std::string_view mode = argv[2];
        if (mode == "collector-loss" || mode == "fault-after-loss")
            supple::diagnostics::detail::setSupervisorHook(crashSupervisorByFault);
        if (mode == "collector-loss-stack")
            supple::diagnostics::detail::setSupervisorHook(crashSupervisorByStack);
        if (mode == "collector-loss-abort")
            supple::diagnostics::detail::setSupervisorHook(crashSupervisorByAbort);
        if (mode == "supervisor-killed-hung")
            supple::diagnostics::detail::setStopBound(std::chrono::milliseconds(300));
    }
#if !defined(_WIN32)
    if (std::string_view(argv[2]) == "handled") std::signal(SIGSEGV, priorSignal);
#endif
    if (std::string_view(argv[2]) == "prior-terminate" ||
        std::string_view(argv[2]) == "prior-terminate-throw")
        std::set_terminate(priorTerminate);
#if defined(_WIN32)
    if (std::string_view(argv[2]) == "prior-filter" ||
        std::string_view(argv[2]) == "prior-filter-resume" ||
        std::string_view(argv[2]) == "prior-filter-cpp-code") {
        filterResumes = std::string_view(argv[2]) == "prior-filter-resume";
        SetUnhandledExceptionFilter(priorFilter);
    }
#endif
#if defined(_WIN32)
    // The Windows collector cannot be tried on the machines this is written on: it says
    // what it saw on the supervisor's stderr, where the lifecycle test shows it.
    supple::diagnostics::detail::setTraceEvidence(
        [](const char* line) { std::fputs(line, stderr); });
#endif
    return supple::diagnostics::runFrontend(argc, argv, supple::diagnostics::Role::Fixture, body,
                                            {std::filesystem::path(argv[1])});
}
