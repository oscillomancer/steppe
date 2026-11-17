// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
// The plug-in inventory of a crash report (ADR-0083, topic 5c).
//
// Worker side: the frontend installs a sink in diagctx that turns each announcement into an
// Instance packet and sends it without ever waiting. A full channel, a closed gate (a terminal
// handoff has begun) or a failed send drops the event and counts it; the count travels on the
// packets that do get through, and on the terminal packet.
//
// Supervisor side: `Inventory` applies the packets it receives and, when a report is written,
// says what each cell of the execution table was. Every packet is untrusted: a damaged
// record, an unknown cell or an event for an instance it never heard of is counted and
// changes nothing, so no entry is ever invented. The plug-in's own strings are checked again
// here (valid UTF-8, no control characters, at most 64 bytes) before they reach a report.
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>
#include "native.h"
#include "supple/diagctx/inventory.h"
#include "supple/json/json.h"
#include "trace.h"
namespace supple::diagnostics::detail {

// ---- worker side -------------------------------------------------------------------------

// Sends this process's announcements to the supervisor over `native`, tagged with `boot`'s
// incident and role. Once, after the Ready packet; later calls do nothing.
void installInventory(const Native& native, const Packet& boot);
// A terminal handoff is about to use the channel: no further event is sent, and the ones
// being sent are given a moment (bounded) to finish. Async-signal-safe.
void closeInventory() noexcept;
// Tests only: forgets the installed sink, the closed gate and the drop count, and replaces the
// function that sends a packet (null restores publish()).
void resetInventoryForTests(bool (*publishFn)(Native&, const Packet&) noexcept);
// Events dropped so far. Async-signal-safe.
std::uint32_t inventoryDropped() noexcept;

// ---- supervisor side ---------------------------------------------------------------------

constexpr std::size_t kMaxPlugins = diagctx::kMaxInstances;  // live: one per cell
constexpr std::size_t kRetiredPlugins = 64;                  // destroyed, kept for the report

struct PluginEntry {
    std::uint32_t ordinal = 0;  // the report's id: order of first announcement
    std::uint32_t cell = 0;
    std::uint16_t generation = 0;
    diagctx::InstanceKind kind = diagctx::InstanceKind::None;
    diagctx::InstanceOrigin origin = diagctx::InstanceOrigin::None;
    diagctx::InstanceMode mode = diagctx::InstanceMode::None;
    diagctx::InstanceState state = diagctx::InstanceState::None;
    std::uint64_t image = 0;                             // never reported
    std::array<std::string, diagctx::kTextFields> text;  // name, vendor, version, id; checked
};

// The text a report may show for a plug-in's string: at most `kMaxText` bytes, valid UTF-8,
// every control character (C0, DEL, C1) and every byte that is not UTF-8 replaced by '?'.
std::string cleanText(const char* bytes, std::size_t length);

class Inventory {
  public:
    Inventory();
    // Applies an Instance packet. False, with the packet counted and nothing changed, when
    // its record is damaged or names an instance that was never announced.
    bool apply(const Packet& packet);
    // The worker's own count of events it could not send (the largest seen).
    void noteDropped(std::uint32_t workerCount) noexcept;

    bool empty() const noexcept { return announced_ == 0; }
    // Damaged or unmatched packets, and events the worker said it dropped.
    std::size_t rejected() const noexcept { return rejected_; }
    std::size_t workerDropped() const noexcept { return workerDropped_; }
    // Destroyed instances that fell out of the retired ring.
    std::size_t retiredLost() const noexcept { return retiredLost_; }

    // The entry of a cell and generation, live first, then the latest retired one; null if the
    // inventory never heard of it.
    const PluginEntry* find(std::uint32_t cell, std::uint16_t generation) const;
    // Live and retired entries by ordinal.
    std::vector<const PluginEntry*> entries() const;

    // The report's "plugins" array. `trace`, when there is one, supplies the module each
    // instance came from (`build`) and which thread was inside which instance (`executing`).
    json::Value pluginsJson(const TraceSnapshot* trace) const;
    // Limit strings for the report's "unavailable".
    std::vector<std::string> limits() const;

  private:
    void retire(PluginEntry entry);
    std::vector<std::optional<PluginEntry>> live_;
    std::deque<PluginEntry> retired_;
    std::uint32_t nextOrdinal_ = 0;
    std::size_t announced_ = 0, rejected_ = 0, workerDropped_ = 0, retiredLost_ = 0;
};

// Strict validation of a report's "plugins" value; `threadCount` is the report's number of
// threads (an `executing` entry names one of them).
bool pluginsShape(const json::Value& plugins, std::size_t threadCount);

}  // namespace supple::diagnostics::detail
