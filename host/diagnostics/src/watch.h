// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <chrono>
#include "supple/diagcap/native.h"
// The worker's watch on its supervisor (ADR-0091): tells a collector's crash from a
// launcher's stop, and gives the second the program's own normal stop.
namespace supple::diagnostics::detail {
// How long the program has to end after the stop request before it is ended hard.
constexpr std::chrono::seconds kStopBound{10};
// Worker, once, after the Ready packet: starts the watcher thread. It is never on the audio
// path, is not in the execution table (a report shows it as an `other` thread parked in
// the wait), and takes no part once a terminal handler has begun. Does nothing for a
// channel that is not trusted.
void startSupervisorWatch(const Native& native) noexcept;
// What the watcher concluded; Pending until it has.
SupervisorEnd supervisorEnd() noexcept;
const char* supervisorEndName(SupervisorEnd end) noexcept;
// Tests only. The bound the watcher waits after the stop request (default kStopBound), and
// a function the supervisor calls with every packet it takes from the worker, so a test
// can make the supervisor crash on its own, which no portable outside action can.
void setStopBound(std::chrono::milliseconds bound) noexcept;
using SupervisorHook = void (*)(const Packet& packet);
void setSupervisorHook(SupervisorHook hook) noexcept;
SupervisorHook supervisorHook() noexcept;
// Tests only: called by the supervisor after it applied each inventory packet (accepted or not),
// with the inventory as it is then, so a test can look at what the supervisor holds (a bridge's
// capture slot, topic 6c) from inside the supervisor process, where nothing else can.
class Inventory;
using InventoryHook = void (*)(const Packet& packet, const Inventory& inventory);
void setInventoryHook(InventoryHook hook) noexcept;
InventoryHook inventoryHook() noexcept;
}  // namespace supple::diagnostics::detail
