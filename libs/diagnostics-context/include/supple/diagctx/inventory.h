// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The plug-in inventory (ADR-0083, topic 5c): what each cell of the execution table is.
//
// A cell says that some thread is inside an entry of some instance; the inventory says what
// that instance is. The host announces each instance and library when it is created, when its
// state changes and when it is gone; the diagnostics frontend, which this library does not
// know, installs a sink that sends the announcements to the supervisor. With no sink
// installed (a test, a tool, a process that runs no supervisor) an announcement does nothing.
//
// Everything here is cold: it runs on whatever thread creates, activates or destroys an
// instance, never on an audio path, and it may allocate, lock and write to a socket. Nothing
// here is marked SUPPLE_RT, and no real-time code may call it. The threads that announce are
// the ones that create, activate, deactivate and destroy: the model thread, the plan compiler,
// a scan or sandbox session thread; never `process`, `reset`, `flush` or start/stop
// processing. Two checks hold that: clang's function-effects analysis rejects a SUPPLE_RT
// function that calls these (they carry no `nonblocking`), and every call takes a lock,
// sink or none, so the `rtsan` preset aborts one reached from an audio path even in a process
// with no sink (tests/rt-safety/rtsan_announce_check.cpp).
//
// What an announcement may carry. The plug-in's own strings (name, vendor, version, id) are
// untrusted text: the sink bounds them to `kMaxText` bytes and the supervisor checks them
// again before they reach a report. The host never passes a path, a project name, a
// parameter, a preset or anything a user typed. `image` is an address inside the plug-in's
// binary (the address of a function it exports); it is private to the channel, never
// recorded, and lets the supervisor say which of the process's modules the instance came
// from.
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace supple::diagctx {

// The closed sets below are the report's contract; None is never announced.
enum class InstanceKind : std::uint8_t {
    None = 0,
    Clap,
    Vst3,
    ClapLibrary,  // a loaded CLAP binary (one cell for the library, one for each instance)
    Vst3Library,
    Count_
};
enum class InstanceOrigin : std::uint8_t {
    None = 0,
    Foreign,
    Host,
    Stock,
    Library,
    Project,
    Count_
};
enum class InstanceMode : std::uint8_t { None = 0, InProcess, Bridged, Count_ };
enum class InstanceState : std::uint8_t { None = 0, Loading, Active, Inactive, Destroyed, Count_ };
constexpr std::uint8_t kInstanceKindCount = static_cast<std::uint8_t>(InstanceKind::Count_);
constexpr std::uint8_t kInstanceOriginCount = static_cast<std::uint8_t>(InstanceOrigin::Count_);
constexpr std::uint8_t kInstanceModeCount = static_cast<std::uint8_t>(InstanceMode::Count_);
constexpr std::uint8_t kInstanceStateCount = static_cast<std::uint8_t>(InstanceState::Count_);
// The report strings ("clap", "foreign", "in_process", "loading", ...); nullptr for None or
// out of range.
const char* instanceKindName(InstanceKind kind) noexcept;
const char* instanceOriginName(InstanceOrigin origin) noexcept;
const char* instanceModeName(InstanceMode mode) noexcept;
const char* instanceStateName(InstanceState state) noexcept;

// Each of a plug-in's four strings is at most this many bytes in a report.
constexpr std::size_t kMaxText = 64;
constexpr std::size_t kTextFields = 4;  // name, vendor, version, id

// What an announcement says about an instance. A string longer than kMaxText is cut.
struct Identity {
    std::string_view name, vendor, version, id;
};

enum class EventKind : std::uint8_t {
    Add = 1,  // the instance exists (again, with new facts: the whole description replaces)
    State,    // its state changed
    Remove,   // it is gone
    Names,    // four command names (topic 5d): `cell` is the id of the first, `identity` holds them
    Bridge,   // a bridge process started or ended (topic 6a): `bridge`, and for an end
              // `how`/`status`
    Child,    // a bridged instance's identity inside its bridge (topic 6b): `cell` and
              // `generation` are the host's own, `childCell`, `childGeneration` and `image` the
              // bridge process's
};
// How a bridge process ended, as the host classified it when it observed the end (topic 6a).
// None is the other event: the bridge started. Host: the host killed it (a hung or abandoned
// bridge, a protocol violation, a teardown), so its status says nothing about the plug-in.
// Exit: it ended by itself with an exit status. Signal and Exception: the operating system
// ended it (POSIX signal; Windows exception status).
enum class BridgeHow : std::uint8_t { None = 0, Exit, Signal, Exception, Host, Count_ };
constexpr std::uint8_t kBridgeHowCount = static_cast<std::uint8_t>(BridgeHow::Count_);
// The report strings ("exit", "signal", "exception", "host"); nullptr for None or out of range.
const char* bridgeHowName(BridgeHow how) noexcept;
struct Event {
    EventKind event = EventKind::Add;
    std::uint32_t cell = 0;
    std::uint16_t generation = 0;  // the cell's generation when the event was made
    InstanceKind kind = InstanceKind::None;
    InstanceOrigin origin = InstanceOrigin::None;
    InstanceMode mode = InstanceMode::None;
    InstanceState state = InstanceState::None;
    std::uint64_t image = 0;
    // The bridge process the instance runs in: 0 is this process, n >= 1 the n-th bridge process
    // the host started, never reused (topic 6a). For a Bridge event, the bridge it is about.
    std::uint16_t bridge = 0;
    BridgeHow how = BridgeHow::None;  // Bridge events: None = started, else how it ended
    std::int32_t status = 0;          // Bridge events, an end: the native status (signal, code)
    // Child events (topic 6b): the instance's cell and generation in the bridge process's own
    // table, and (in `image`) an address in the plug-in's binary there. Private to the channel:
    // the supervisor matches a bridge's capsule to the instance with them and never records them.
    std::uint32_t childCell = 0;
    std::uint16_t childGeneration = 0;
    Identity identity;
};

// The sink: called synchronously on the announcing thread, which holds none of this library's
// locks (there are none). Set while cold; null removes it. Must not throw.
using InventorySink = void (*)(const Event&) noexcept;
void setInventorySink(InventorySink sink) noexcept;

// Announcements. A `cell` of kNoCell (an instance the table had no room for) is ignored: it has
// no word for a crash to find, so the report has nothing to match it with. The generation is
// read from the cell here, so a caller says nothing about it.
void announce(std::uint32_t cell, InstanceKind kind, InstanceOrigin origin, InstanceMode mode,
              InstanceState state, std::uint64_t image, const Identity& identity,
              std::uint16_t bridge = 0) noexcept;
// A bridge process started (`how` None) or ended (anything else, with its native `status`).
// `bridge` is an ordinal of 1 or more; an announcement of 0 is ignored. Called from the thread
// that spawns the bridge and from the main thread that observes its end, never from an audio
// path (it takes the same lock every announcement does).
void announceBridge(std::uint16_t bridge, BridgeHow how = BridgeHow::None,
                    std::int32_t status = 0) noexcept;
void announceState(std::uint32_t cell, InstanceState state) noexcept;
// What a bridged instance is in its bridge process (topic 6b): `cell` is the host's cell of the
// instance, `childCell` its cell in the bridge's table, `childGeneration` that cell's generation
// there and `childImage` an address in the plug-in's binary in the bridge's address space. Cold,
// from the thread that created the instance, after the bridge answered Create. Ignored when
// either cell is out of range (the table had no room for it).
void announceChild(std::uint32_t cell, std::uint32_t childCell, std::uint16_t childGeneration,
                   std::uint64_t childImage) noexcept;
// The generation `cell` has now: what an announcement made this moment would carry. 0 for a cell
// out of range. The bridge says it in its Create reply (topic 6b).
std::uint16_t cellGeneration(std::uint32_t cell) noexcept;
// The image `announce` last gave for `cell` (0: none, or a cell out of range). It stays after
// the instance is removed, until the cell is announced again: a retired instance's module is
// still one the report names. Atomic loads only, so a terminal handler may call it. macOS,
// whose handler lists the process's images itself, adds the module of each of these to its
// capsule so a loaded plug-in that is on no stack still has a build.
std::uint64_t announcedImage(std::uint32_t cell) noexcept;
// The instance is gone; call before the cell is given back.
void announceRemove(std::uint32_t cell) noexcept;

}  // namespace supple::diagctx
