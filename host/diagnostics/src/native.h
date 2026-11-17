// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "supple/base/result.h"
namespace supple::diagnostics::detail {
enum class Message : std::uint32_t { Boot = 1, Ready = 2, Expected = 3, Terminal = 4, Trace = 5 };
struct Packet {
    std::uint32_t magic = 0x43525348, version = 1;
    Message kind = Message::Boot;
    std::uint32_t role = 0, code = 0, cause = 0;
    // Terminal packets also carry the faulting thread's id and stack pointer. They are
    // private to the channel; the collector reads memory with them and the report
    // never records them.
    std::uint64_t pid = 0, pc = 0, frame = 0, tid = 0, sp = 0;
    std::array<char, 33> incident{};
};
// A Ready packet reuses the `pc` field for the address of the worker's execution table
// (topic 5); it is not a program counter. Use these two, never `pc`, for that address.
// The supervisor keeps it and reads the table from the worker only when a terminal packet
// arrives. It is the worker's own address, meaningful only to a bounded remote read. If a
// later topic adds a Table message, the address moves there.
inline void setTableAddress(Packet& ready, const void* table) noexcept {
    ready.pc = reinterpret_cast<std::uintptr_t>(table);
}
inline std::uint64_t tableAddress(const Packet& ready) noexcept { return ready.pc; }
static_assert(sizeof(Packet) <= 128);
// The Windows channel keeps one shared slot per message kind and a bit mask of the ones
// published and not yet taken. The slot of a kind, and which pending slot the supervisor
// takes next, live here so a portable test can pin the order.
inline unsigned sharedSlot(Message kind) noexcept {
    return kind == Message::Ready ? 0U : kind == Message::Expected ? 1U : 2U;
}
// The next slot to take from `pending` (bit n set: slot n is published and untaken), or -1.
// Ready comes before Terminal: a worker that publishes Ready and faults at once can have
// both bits set when the supervisor wakes, and the table address Ready carries must be
// known before the Terminal packet is handled, which reads the table. Terminal comes
// before Expected, which a dying worker never needs read first.
inline int nextSharedSlot(std::uint32_t pending) noexcept {
    for (const unsigned slot :
         {sharedSlot(Message::Ready), sharedSlot(Message::Terminal), sharedSlot(Message::Expected)})
        if (pending & (1U << slot)) return static_cast<int>(slot);
    return -1;
}
struct Native {
    std::intptr_t channel = -1, process = -1, event = -1, ack = -1;
    void* shared = nullptr;
    std::uint32_t seen = 0;
    bool trusted = true;
    // POSIX caches waitpid observation independently of channel ownership.
    bool exited = false, statusKnown = false;
    int status = 0;
    // Linux: a trace left a thread of the worker attached (it never reached its stop
    // before the deadline). Only this supervisor can reap such a thread once it dies,
    // and the worker is not reported until it has; wait() and receive() drain them.
    bool traced = false;
    // The worker's execution table (diagctx), from its Ready packet; 0 until then.
    std::uint64_t table = 0;
};
Result<Native> launch(int argc, char** argv, const Packet& boot);
Result<Native> adopt(std::string_view value, Packet& boot);
bool receive(Native& native, Packet& packet);  // collector, parked until work/exit
bool publish(Native& native, const Packet& packet) noexcept;
void acknowledge(Native& native) noexcept;
void awaitAcknowledgement(Native& native) noexcept;  // terminal only, <=1 second
inline bool windowsFatalStatus(std::uint32_t code, std::optional<std::uint32_t> expected) {
    // A Main return can use every DWORD value, including exception-shaped
    // values. The caller withholds expected when a terminal capsule exists.
    return (!expected || *expected != code) && (code & 0xc0000000U) == 0xc0000000U;
}
int wait(Native& native, bool& fatal, std::optional<std::uint32_t> expected = {});
// What a worker concluded when its supervisor went away (ADR-0091). Pending until it has
// looked. Collector: the supervisor's own fatal handler said "collector failed" first, so
// the worker keeps running. Launcher: the supervisor vanished without saying so (SIGKILL,
// TerminateProcess, a launcher's hard kill), which is a stop request. Unknown: the channel
// showed something else, or a terminal handler is already running; the worker does nothing.
enum class SupervisorEnd : std::uint8_t { Pending, Collector, Launcher, Unknown };
// Worker side. Parks the calling thread until the supervisor has gone away or the channel
// shows anything else, and says which. Never consumes a byte from the channel and never
// depends on the descriptor's blocking mode. Never returns Pending.
SupervisorEnd awaitSupervisorEnd(const Native& native) noexcept;
// Supervisor side. Installs the supervisor's own fatal handlers: each tells the worker
// "collector failed" and then lets the process die as it would have. Once, after launch.
void announceCollectorFailure(const Native& native) noexcept;
// Worker side. What a launcher's stop does: the same as a forwarded SIGTERM (POSIX
// `kill(getpid(), SIGTERM)`, Windows `raise(SIGTERM)`), and the hard end after the bound
// (`_exit(128 + SIGTERM)`, `TerminateProcess`). Neither runs a crash handler.
void requestStop() noexcept;
[[noreturn]] void hardExit() noexcept;
void close(Native& native);
void closeProtocolStdio();
std::uint64_t pid();
Result<std::string> incidentId();
bool valid(const Packet& packet);
}  // namespace supple::diagnostics::detail
