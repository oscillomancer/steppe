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
enum class Message : std::uint32_t {
    Boot = 1,
    Ready = 2,
    Expected = 3,
    Terminal = 4,
    Trace = 5,
    Instance = 6  // one inventory event (topic 5c), carried in `Packet::instance`
};
// Version 2 added `dropped` and `instance` (topic 5c) and the Instance message. Version 3 added
// `Packet::resource` and the one event that may carry a resource (topic 6c). Both ends are the
// same executable, so a mismatch is only a damaged or foreign packet.
constexpr std::uint32_t kProtocolVersion = 3;
// One inventory event as it travels (topic 5c): the whole of what an announcement says, in
// fixed-size plain fields so a packet stays one fixed-size record. Everything in it is
// untrusted on receipt (the supervisor checks every field before use; see inventory.h).
struct InstanceRecord {
    std::uint32_t cell = 0;
    std::uint16_t generation = 0;
    std::uint8_t event = 0;  // diagctx::EventKind
    std::uint8_t kind = 0, origin = 0, mode = 0, state = 0;
    std::uint8_t length[4]{};  // bytes used of each text field: name, vendor, version, id
    // Topic 6a. `bridge`: the bridge process the instance runs in (0: this process). A Bridge
    // event (`event` = diagctx::EventKind::Bridge) is about bridge `bridge`; `how` is
    // diagctx::BridgeHow (0 = started) and `status` the native status of an end.
    std::uint16_t bridge = 0;
    std::uint8_t how = 0;
    std::int32_t status = 0;
    std::uint64_t image = 0;  // private: an address inside the plug-in's binary
    // Topic 6b. A Child event (`event` = diagctx::EventKind::Child) names the instance by `cell`
    // and `generation` and says where it is inside its bridge process: its cell and generation
    // in that process's table, and in `image` an address of the plug-in's binary there.
    std::uint32_t childCell = 0;
    std::uint16_t childGeneration = 0;
    char text[4][64]{};
};
struct Packet {
    std::uint32_t magic = 0x43525348, version = kProtocolVersion;
    Message kind = Message::Boot;
    std::uint32_t role = 0, code = 0, cause = 0;
    // How many inventory events the worker could not send so far (a full channel, a closed
    // gate). Meaningful on Instance and Terminal packets; the supervisor keeps the largest
    // value it saw.
    std::uint32_t dropped = 0;
    // Terminal packets also carry the faulting thread's id and stack pointer. They are
    // private to the channel; the collector reads memory with them and the report
    // never records them.
    std::uint64_t pid = 0, pc = 0, frame = 0, tid = 0, sp = 0;
    // Windows: the sender's own handle value for the one resource this packet may carry
    // (`mayCarryResource`), which the receiver duplicates from the sender's process; 0 for none.
    // Always 0 on POSIX, where the descriptor travels as ancillary data beside the packet.
    std::uint64_t resource = 0;
    std::array<char, 33> incident{};
    InstanceRecord instance;
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
// The one event that carries a resource (topic 6c): diagctx::EventKind::Slot, a bridge's capture
// slot. inventory.cpp asserts the number. A resource on any other packet is damage: the receiver
// closes it (never uses it) and counts it in `Native::refusedResources`.
constexpr std::uint8_t kSlotEventKind = 7;
inline bool mayCarryResource(const Packet& packet) noexcept {
    return packet.kind == Message::Instance && packet.instance.event == kSlotEventKind;
}
// A packet is written to a stream socket in one non-blocking send; it stays well under the
// smallest atomic pipe write (512 bytes, macOS) so one send takes it or none of it.
static_assert(sizeof(Packet) <= 512);
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
// Inventory events (topic 5c) are many and in order, so the Windows channel keeps them in a ring
// of whole packets, apart from the three one-per-kind slots above: the worker advances `head`
// after it has filled a slot (release), the supervisor advances `tail` after it has copied one
// out (release). A full ring drops the event (the worker counts it). The ring has one producer
// at a time: callers of publish() serialize Instance packets themselves.
constexpr std::uint32_t kInventoryRing = 256;
// POSIX publish(): the milliseconds (by the monotonic clock, polled 1 ms at a time) a part-sent
// packet is given to be finished, so the longest a send can be in progress. The terminal path waits
// out the same bound. (Only where there is no clock does it count that many polls.)
constexpr int kPartSendPolls = 250;
inline bool ringHasRoom(std::uint32_t head, std::uint32_t tail) noexcept {
    return head - tail < kInventoryRing;  // unsigned: the counters wrap
}
inline bool ringHasData(std::uint32_t head, std::uint32_t tail) noexcept { return head != tail; }
inline std::uint32_t ringSlot(std::uint32_t counter) noexcept { return counter % kInventoryRing; }

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
    // Worker, POSIX: a publish() sent part of a packet and could not send the rest, so the
    // stream no longer holds whole packets. The caller must stop sending on it.
    bool torn = false;
    // Supervisor: resources that arrived and were closed unused, or could not be taken: a second
    // descriptor, one on a packet that may carry none (or not on its first byte), control data of
    // another kind, a handle that could not be duplicated. Only ever grows.
    std::uint32_t refusedResources = 0;
};
Result<Native> launch(int argc, char** argv, const Packet& boot);
Result<Native> adopt(std::string_view value, Packet& boot);
// Collector, parked until work/exit. `resource`, when given, receives the resource a packet that
// may carry one (`mayCarryResource`) brought: a descriptor on POSIX, a handle duplicated from the
// worker on Windows, owned by the caller; -1 when it brought none. Anything else that arrived is
// closed here and counted (`refusedResources`); with no `resource` a packet's resource is one of
// those. Never leaves `*resource` set when it returns false.
bool receive(Native& native, Packet& packet, std::intptr_t* resource = nullptr);
bool publish(Native& native, const Packet& packet) noexcept;
// publish() with one resource, a descriptor or a handle of this process, for a packet that may
// carry one. POSIX: the descriptor goes as SCM_RIGHTS attached to the packet's first byte, so a
// send that is finished later still delivers it; Windows: the handle value goes in
// `packet.resource`. Never waits, as publish() never does. A negative `resource` is publish().
bool publishWithResource(Native& native, const Packet& packet, std::intptr_t resource) noexcept;
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
