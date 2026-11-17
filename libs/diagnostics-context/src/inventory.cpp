// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/inventory.h"

#include <atomic>

#include "supple/diagctx/scope.h"

namespace supple::diagctx {
namespace {
std::atomic<InventorySink> gSink{nullptr};
static_assert(std::atomic<InventorySink>::is_always_lock_free);

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

void setInventorySink(InventorySink sink) noexcept { gSink.store(sink, std::memory_order_release); }

void announce(std::uint32_t cell, InstanceKind kind, InstanceOrigin origin, InstanceMode mode,
              InstanceState state, std::uint64_t image, const Identity& identity) noexcept {
    const auto sink = gSink.load(std::memory_order_acquire);
    if (!sink || cell >= kMaxInstances) return;
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
    const auto sink = gSink.load(std::memory_order_acquire);
    if (!sink || cell >= kMaxInstances) return;
    Event event;
    event.event = EventKind::State;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.state = state;
    sink(event);
}

void announceRemove(std::uint32_t cell) noexcept {
    const auto sink = gSink.load(std::memory_order_acquire);
    if (!sink || cell >= kMaxInstances) return;
    Event event;
    event.event = EventKind::Remove;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.state = InstanceState::Destroyed;
    sink(event);
}

}  // namespace supple::diagctx
