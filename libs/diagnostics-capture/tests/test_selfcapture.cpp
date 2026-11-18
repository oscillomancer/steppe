// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// A bridge's self-capture (topic 6c.2): the slot's protocol, the capsule a fault writes into
// it, and the handlers. On POSIX each fault happens for real, in a forked child that has the
// handlers installed and a shared mapping as its slot; the parent reads what the child left
// and the child's own end (the status must stay the fatal one). One child runs under a seccomp
// filter that kills it on any system call but the few the handler may make, which is the
// evidence that it allocates, locks, opens and writes nothing. Windows and macOS handlers are
// proven only by native CI.
#include <doctest/doctest.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include "supple/diagcap/selfcapture.h"
#include "supple/diagcap/trace.h"
#include "supple/diagctx/scope.h"
#include "supple/diagctx/table.h"
#if !defined(_WIN32)
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <exception>
#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif
#endif
using namespace supple::diagnostics::selfcapture;
namespace diagctx = supple::diagctx;
namespace detail = supple::diagnostics::detail;
namespace {
struct Parsed {
    detail::CapsuleHeader header{};
    std::vector<BridgeModule> modules;
    std::vector<detail::CapsuleThread> heads;
    std::vector<std::vector<std::uint64_t>> frames;
    diagctx::TableImage table{};
    std::size_t consumed = 0;
};
// Reads a complete slot the way its reader must: the header first, then exactly `length`.
bool parse(const unsigned char* memory, Parsed& out) {
    SlotHeader slot;
    std::memcpy(&slot, memory, sizeof(slot));
    if (!slotUsable(slot, kSlotBytes) || slot.state != kSlotComplete) return false;
    const unsigned char* at = memory + sizeof(slot);
    const unsigned char* end = at + slot.length;
    const auto take = [&](void* to, std::size_t n) {
        if (static_cast<std::size_t>(end - at) < n) return false;
        std::memcpy(to, at, n);
        at += n;
        return true;
    };
    if (!take(&out.header, sizeof(out.header)) || out.header.magic != kBridgeCapsuleMagic)
        return false;
    out.modules.resize(out.header.moduleCount);
    for (auto& m : out.modules)
        if (!take(&m, sizeof(m))) return false;
    for (std::uint32_t i = 0; i < out.header.threadCount; ++i) {
        detail::CapsuleThread head;
        if (!take(&head, sizeof(head))) return false;
        std::vector<std::uint64_t> frames(head.frameCount);
        if (head.frameCount > 0 && !take(frames.data(), frames.size() * sizeof(std::uint64_t)))
            return false;
        out.heads.push_back(head);
        out.frames.push_back(std::move(frames));
    }
    if (!(out.header.flags & kCapsuleHasTable) || !take(&out.table, sizeof(out.table)))
        return false;
    out.consumed = static_cast<std::size_t>(at - (memory + sizeof(slot)));
    return at == end;
}

TEST_CASE("the slot header is written and checked field by field") {
    alignas(64) static unsigned char memory[kSlotBytes];
    std::memset(memory, 0, sizeof(memory));
    CHECK(initializeSlot(memory, kSlotBytes, 7));
    SlotHeader header;
    std::memcpy(&header, memory, sizeof(header));
    CHECK(slotUsable(header, kSlotBytes));
    CHECK(header.state == kSlotEmpty);
    CHECK(header.ordinal == 7);
    SUBCASE("not a slot unless every field is what the reader knows") {
        auto bad = header;
        bad.magic ^= 1;
        CHECK_FALSE(slotUsable(bad, kSlotBytes));
        bad = header;
        bad.version += 1;
        CHECK_FALSE(slotUsable(bad, kSlotBytes));
        bad = header;
        bad.capacity -= 1;
        CHECK_FALSE(slotUsable(bad, kSlotBytes));
        bad = header;
        bad.length = static_cast<std::uint32_t>(kSlotBytes);  // more than fits after the header
        CHECK_FALSE(slotUsable(bad, kSlotBytes));
        CHECK_FALSE(slotUsable(header, kSlotBytes - 1));
        CHECK_FALSE(slotUsable(header, kSlotBytes + 1));
    }
    SUBCASE("a slot is only made on memory of exactly the expected size") {
        CHECK_FALSE(initializeSlot(nullptr, kSlotBytes, 1));
        CHECK_FALSE(initializeSlot(memory, kSlotBytes - 1, 1));
        CHECK_FALSE(initializeSlot(memory, kSlotBytes * 2, 1));
        CHECK_FALSE(initializeSlot(memory + 1, kSlotBytes, 1));
    }
    SUBCASE("attachSlot refuses what is not an empty, initialized slot of the right size") {
        CHECK_FALSE(attachSlot(nullptr, kSlotBytes));
        CHECK_FALSE(attachSlot(memory, kSlotBytes - 1));
        CHECK_FALSE(attachSlot(memory + 1, kSlotBytes));
        std::memset(memory, 0, sizeof(memory));  // never initialized
        CHECK_FALSE(attachSlot(memory, kSlotBytes));
        CHECK(initializeSlot(memory, kSlotBytes, 1));
        reinterpret_cast<SlotHeader*>(memory)->state = kSlotComplete;  // written already
        CHECK_FALSE(attachSlot(memory, kSlotBytes));
    }
}

TEST_CASE("the largest capsule fits the slot and the sizes are the contract's") {
    CHECK(sizeof(SlotHeader) == 64);
    CHECK(sizeof(BridgeModule) == 64);
    CHECK(diagctx::kTableBytes == 74816);
    CHECK(sizeof(SlotHeader) + kBridgeCapsuleMaxBytes <= kSlotBytes);
}

#if !defined(_WIN32)
// ---- children -------------------------------------------------------------------------------
constexpr int kSkip = 77, kBad = 78, kNoFault = 80;
unsigned char* sharedSlot(std::uint32_t ordinal) {
    void* memory =
        ::mmap(nullptr, kSlotBytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    REQUIRE(memory != MAP_FAILED);
    REQUIRE(initializeSlot(memory, kSlotBytes, ordinal));
    return static_cast<unsigned char*>(memory);
}
// A child that is another process image of this program with the handlers on: the signals
// doctest installs for a test case are put back to their defaults first, since the handlers
// install only over a default action.
template <class Body>
int runChild(Body body) {
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        for (const int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) ::signal(s, SIG_DFL);
        body();
        ::_exit(kNoFault);
    }
    int status = 0;
    REQUIRE(::waitpid(pid, &status, 0) == pid);
    return status;
}
__attribute__((noinline)) void markerFunction() { asm volatile(""); }
__attribute__((noinline)) void crashHere() {
    *static_cast<volatile int*>(nullptr) = 1;
    asm volatile("");
}
#if defined(__linux__)
// Kills the process on any system call but those the handler may make (and the end of the
// process): no memory, no lock (futex), no file, no write. process_vm_readv is its reader.
bool restrictSyscalls() {
    static const unsigned allowed[] = {SYS_process_vm_readv, SYS_getpid,       SYS_gettid,
                                       SYS_clock_gettime,    SYS_rt_sigaction, SYS_rt_sigprocmask,
                                       SYS_tgkill,           SYS_tkill,        SYS_kill,
                                       SYS_exit_group,       SYS_exit,         SYS_rt_sigreturn,
                                       SYS_sigaltstack};
#if defined(__x86_64__)
    constexpr unsigned arch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
    constexpr unsigned arch = AUDIT_ARCH_AARCH64;
#else
    return false;
#endif
    std::vector<sock_filter> program;
    const auto add = [&](sock_filter f) { program.push_back(f); };
    add(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
    add(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, arch, 1, 0));
    add(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    add(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
    for (const unsigned nr : allowed) {
        add(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 1));
        add(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW));
    }
    add(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
    sock_fprog fprog{static_cast<unsigned short>(program.size()), program.data()};
    return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
           ::syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fprog) == 0;
}
#endif

// The common setup of a child that will fail: the handlers, a registered thread, the
// recorded executable, the shared slot. Exits with kSkip where no handler can be installed.
void prepare(unsigned char* memory) {
    if (!install()) ::_exit(kSkip);
    if (!diagctx::registerThread(diagctx::ThreadRole::BridgeControl)) ::_exit(kBad);
    if (!recordImage(reinterpret_cast<const void*>(&markerFunction))) ::_exit(kBad);
    if (!attachSlot(memory, kSlotBytes)) ::_exit(kBad);
}

// What every captured fault must show.
void checkCapsule(const Parsed& p, bool expectEntry, std::uint32_t cell) {
    CHECK(p.header.threadCount == 1);
    REQUIRE(p.heads.size() == 1);
    CHECK(p.heads[0].role == static_cast<std::uint8_t>(detail::Role::Fault));
    CHECK(p.heads[0].missing == static_cast<std::uint8_t>(detail::Missing::None));
    CHECK(p.heads[0].registered == static_cast<std::uint8_t>(diagctx::ThreadRole::BridgeControl));
    REQUIRE_FALSE(p.frames[0].empty());
    CHECK(p.header.commandCount == 0);
    // The table copy is a table this build wrote, and it knows the fault thread by the id the
    // header carries.
    CHECK(diagctx::imageValid(p.table));
    const auto found = diagctx::findThread(p.table, p.header.reserved);
    CHECK(found.role == diagctx::ThreadRole::BridgeControl);
    if (expectEntry) {
        CHECK(p.heads[0].entry == static_cast<std::uint8_t>(diagctx::Entry::Create));
        CHECK(p.heads[0].entryCell == cell);
    }
}
// The frame 0 is in the module that holds the test's own code, with an identity or none.
void checkModule(const Parsed& p, std::uint64_t pc) {
    bool inside = false;
    for (const auto& m : p.modules)
        if (pc >= m.start && pc < m.end) {
            inside = true;
            CHECK(m.idLength <= kModuleIdBytes);
            CHECK(m.start < m.end);
        }
    CHECK(inside);
}

TEST_CASE("a real segmentation fault is captured and the process still ends by it") {
    unsigned char* memory = sharedSlot(5);
    const std::uint32_t cell = diagctx::acquireCell();
    const int status = runChild([&] {
        prepare(memory);
        diagctx::MainScope inside(cell, diagctx::Entry::Create);
        crashHere();
    });
    if (WIFEXITED(status) && WEXITSTATUS(status) == kSkip) {
        MESSAGE("no handler installed here (a sanitizer owns the signals): skipped");
        return;
    }
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);  // the exit status stays the fatal one
    Parsed p;
    REQUIRE(parse(memory, p));
    checkCapsule(p, true, cell);
    CHECK(p.header.reserved != 0);
    const auto pc = p.frames[0][0];
    const auto fn = reinterpret_cast<std::uintptr_t>(&crashHere);
    CHECK(pc >= fn);
    CHECK(pc < fn + 256);  // frame 0 is the faulting instruction itself
    checkModule(p, pc);
    SlotHeader header;
    std::memcpy(&header, memory, sizeof(header));
    CHECK(header.ordinal == 5);
    ::munmap(memory, kSlotBytes);
}

#if defined(__linux__)
TEST_CASE("the handler makes only the system calls an async-signal-safe one may") {
    unsigned char* memory = sharedSlot(1);
    const int status = runChild([&] {
        prepare(memory);
        if (!restrictSyscalls()) ::_exit(kSkip);
        crashHere();
    });
    if (WIFEXITED(status) && WEXITSTATUS(status) == kSkip) {
        MESSAGE("no handler or no seccomp here: skipped");
        return;
    }
    // A forbidden call would have ended the child with SIGSYS before the slot completed.
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);
    Parsed p;
    CHECK(parse(memory, p));
    ::munmap(memory, kSlotBytes);
}
#endif

TEST_CASE("abort, a raised signal and std::terminate are captured too") {
    struct Case {
        const char* name;
        int signal;
        int how;
    };
    for (const Case c : {Case{"abort", SIGABRT, 0}, Case{"raise SIGFPE", SIGFPE, 1},
                         Case{"terminate", SIGABRT, 2}}) {
        CAPTURE(c.name);
        unsigned char* memory = sharedSlot(2);
        const int status = runChild([&] {
            prepare(memory);
            if (c.how == 0) std::abort();
            if (c.how == 1) ::raise(SIGFPE);
            std::terminate();
        });
        if (WIFEXITED(status) && WEXITSTATUS(status) == kSkip) {
            MESSAGE("skipped (a sanitizer owns the signals)");
            ::munmap(memory, kSlotBytes);
            return;
        }
        REQUIRE(WIFSIGNALED(status));
        CHECK(WTERMSIG(status) == c.signal);
        Parsed p;
        REQUIRE(parse(memory, p));
        checkCapsule(p, false, 0);
        ::munmap(memory, kSlotBytes);
    }
}

TEST_CASE("a second fault in a bridge leaves the first capsule as it was") {
    unsigned char* memory = sharedSlot(3);
    const int status = runChild([&] {
        prepare(memory);
        // The handler has written once; the fault below finds the slot taken and only ends.
        Fault fault;
        fault.tid = diagctx::currentThreadId();
        const auto frame = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
        fault.pc = reinterpret_cast<std::uintptr_t>(&markerFunction);
        fault.sp = frame;
        if (!captureAndPublish(fault, monotonic())) ::_exit(kBad);
        unsigned char before[sizeof(SlotHeader) + 64];
        std::memcpy(before, memory, sizeof(before));
        if (captureAndPublish(fault, monotonic())) ::_exit(kBad);  // written once per bridge
        if (std::memcmp(before, memory, sizeof(before)) != 0) ::_exit(kBad);
        crashHere();
    });
    if (WIFEXITED(status) && WEXITSTATUS(status) == kSkip) return;
    REQUIRE(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGSEGV);
    Parsed p;
    REQUIRE(parse(memory, p));
    CHECK(p.frames[0][0] == reinterpret_cast<std::uintptr_t>(&markerFunction));  // the first one
    ::munmap(memory, kSlotBytes);
}

TEST_CASE("a capsule that is not done within the handler's bound stays writing") {
    unsigned char* memory = sharedSlot(4);
    const int status = runChild([&] {
        if (!attachSlot(memory, kSlotBytes)) ::_exit(kBad);
        diagctx::initTable();
        Fault fault;
        fault.tid = diagctx::currentThreadId();
        fault.pc = reinterpret_cast<std::uintptr_t>(&markerFunction);
        // The clock reading the handler took at its start is a second in the past.
        const bool done = captureAndPublish(fault, monotonic() - 1'000'000'000ULL);
        ::_exit(done ? kBad : 0);
    });
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
    SlotHeader header;
    std::memcpy(&header, memory, sizeof(header));
    CHECK(header.state == kSlotWriting);  // a reader uses nothing
    Parsed p;
    CHECK_FALSE(parse(memory, p));
    ::munmap(memory, kSlotBytes);
}
#endif
}  // namespace
