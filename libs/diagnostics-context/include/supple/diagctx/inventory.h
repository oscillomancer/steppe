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
};
struct Event {
    EventKind event = EventKind::Add;
    std::uint32_t cell = 0;
    std::uint16_t generation = 0;  // the cell's generation when the event was made
    InstanceKind kind = InstanceKind::None;
    InstanceOrigin origin = InstanceOrigin::None;
    InstanceMode mode = InstanceMode::None;
    InstanceState state = InstanceState::None;
    std::uint64_t image = 0;
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
              InstanceState state, std::uint64_t image, const Identity& identity) noexcept;
void announceState(std::uint32_t cell, InstanceState state) noexcept;
// The instance is gone; call before the cell is given back.
void announceRemove(std::uint32_t cell) noexcept;

}  // namespace supple::diagctx
