// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/inventory.h"

#include <atomic>
#include <mutex>

#include "supple/diagctx/scope.h"

namespace supple::diagctx {
namespace {
std::atomic<InventorySink> gSink{nullptr};
static_assert(std::atomic<InventorySink>::is_always_lock_free);

// Every announcement passes through this lock, sink or none. It orders the sink's calls and,
// more to the point, makes an announcement reached from a SUPPLE_RT function abort under
// RealtimeSanitizer even in a process that has no sink (a test, a tool): announcements are
// cold, and the lock is how the rtsan preset proves none is made on an audio path.
std::mutex gGate;

// The sink in force, and the lock held while it runs; null when there is none or the cell
// has no word.
InventorySink enter(std::unique_lock<std::mutex>& lock, std::uint32_t cell) noexcept {
    lock = std::unique_lock(gGate);
    return cell < kMaxInstances ? gSink.load(std::memory_order_acquire) : nullptr;
}

std::uint16_t generationOfCell(std::uint32_t cell) noexcept {
    return unpackWord(table().instances[cell].audio.load(std::memory_order_relaxed)).generation;
}
}  // namespace

const char* instanceKindName(InstanceKind kind) noexcept {
    switch (kind) {
        case InstanceKind::Clap:
            return "clap";
        case InstanceKind::Vst3:
            return "vst3";
        case InstanceKind::ClapLibrary:
            return "clap_library";
        case InstanceKind::Vst3Library:
            return "vst3_library";
        case InstanceKind::None:
        case InstanceKind::Count_:
            break;
    }
    return nullptr;
}
const char* instanceOriginName(InstanceOrigin origin) noexcept {
    switch (origin) {
        case InstanceOrigin::Foreign:
            return "foreign";
        case InstanceOrigin::Host:
            return "host";
        case InstanceOrigin::Stock:
            return "stock";
        case InstanceOrigin::Library:
            return "library";
        case InstanceOrigin::Project:
            return "project";
        case InstanceOrigin::None:
        case InstanceOrigin::Count_:
            break;
    }
    return nullptr;
}
const char* instanceModeName(InstanceMode mode) noexcept {
    switch (mode) {
        case InstanceMode::InProcess:
            return "in_process";
        case InstanceMode::Bridged:
            return "bridged";
        case InstanceMode::None:
        case InstanceMode::Count_:
            break;
    }
    return nullptr;
}
const char* instanceStateName(InstanceState state) noexcept {
    switch (state) {
        case InstanceState::Loading:
            return "loading";
        case InstanceState::Active:
            return "active";
        case InstanceState::Inactive:
            return "inactive";
        case InstanceState::Destroyed:
            return "destroyed";
        case InstanceState::None:
        case InstanceState::Count_:
            break;
    }
    return nullptr;
}

void setInventorySink(InventorySink sink) noexcept {
    std::lock_guard lock(gGate);  // returns once no announcement is still running the old sink
    gSink.store(sink, std::memory_order_release);
}

void announce(std::uint32_t cell, InstanceKind kind, InstanceOrigin origin, InstanceMode mode,
              InstanceState state, std::uint64_t image, const Identity& identity) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock, cell);
    if (!sink) return;
    Event event;
    event.event = EventKind::Add;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.kind = kind;
    event.origin = origin;
    event.mode = mode;
    event.state = state;
    event.image = image;
    event.identity = identity;
    sink(event);
}

void announceState(std::uint32_t cell, InstanceState state) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock, cell);
    if (!sink) return;
    Event event;
    event.event = EventKind::State;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.state = state;
    sink(event);
}

void announceRemove(std::uint32_t cell) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock, cell);
    if (!sink) return;
    Event event;
    event.event = EventKind::Remove;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.state = InstanceState::Destroyed;
    sink(event);
}

}  // namespace supple::diagctx
