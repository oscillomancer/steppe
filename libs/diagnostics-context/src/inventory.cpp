// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/inventory.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <random>
#include <string>

#include <algorithm>

#include "supple/diagctx/commands.h"
#include "supple/diagctx/scope.h"

namespace supple::diagctx {
namespace {
std::atomic<InventorySink> gSink{nullptr};
static_assert(std::atomic<InventorySink>::is_always_lock_free);

// The image each cell was last announced with, kept whether or not a sink is set, so a
// terminal handler can read them without a lock (announcedImage). Zero: none.
std::atomic<std::uint64_t> gImages[kMaxInstances];
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// Every announcement passes through this lock, sink or none. It orders the sink's calls and,
// more to the point, makes an announcement reached from a SUPPLE_RT function abort under
// RealtimeSanitizer even in a process that has no sink (a test, a tool): announcements are
// cold, and the lock is how the rtsan preset proves none is made on an audio path.
std::mutex gGate;

// The sink in force, with the gate held (in `lock`) while it runs; null when there is none.
InventorySink enter(std::unique_lock<std::mutex>& lock) noexcept {
    lock = std::unique_lock(gGate);
    return gSink.load(std::memory_order_acquire);
}

// A report's id: 128 random bits as 32 lowercase hex characters. The id names a file in a private
// folder and the supervisor never overwrites one, so it needs to be unique, not secret; a random
// device that fails is mixed with the clock and a counter and still gives distinct ids.
std::string mintIncident() noexcept {
    static std::atomic<std::uint64_t> counter{0};
    std::uint64_t words[2]{0, 0};
    try {
        std::random_device device;
        for (auto& word : words) word = (std::uint64_t{device()} << 32) | device();
    } catch (...) {
    }
    words[0] ^=
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    words[1] ^= (counter.fetch_add(1, std::memory_order_relaxed) + 1) * 0x9e3779b97f4a7c15ULL;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text(32, '0');
    for (int w = 0; w < 2; ++w)
        for (int i = 0; i < 16; ++i)
            text[static_cast<std::size_t>(w * 16 + i)] = kHex[(words[w] >> (60 - 4 * i)) & 0xf];
    return text;
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

const char* bridgeHowName(BridgeHow how) noexcept {
    switch (how) {
        case BridgeHow::Exit:
            return "exit";
        case BridgeHow::Signal:
            return "signal";
        case BridgeHow::Exception:
            return "exception";
        case BridgeHow::Host:
            return "host";
        case BridgeHow::None:
        case BridgeHow::Count_:
            break;
    }
    return nullptr;
}

void setInventorySink(InventorySink sink) noexcept {
    std::lock_guard lock(gGate);  // returns once no announcement is still running the old sink
    gSink.store(sink, std::memory_order_release);
}

void announce(std::uint32_t cell, InstanceKind kind, InstanceOrigin origin, InstanceMode mode,
              InstanceState state, std::uint64_t image, const Identity& identity,
              std::uint16_t bridge) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock);
    if (cell < kMaxInstances) gImages[cell].store(image, std::memory_order_relaxed);
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
    event.bridge = bridge;
    event.identity = identity;
    sink(event);
}

std::string announceBridge(std::uint16_t bridge, BridgeHow how, std::int32_t status) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock);
    if (!sink || bridge == 0) return {};
    Event event;
    event.event = EventKind::Bridge;
    event.bridge = bridge;
    event.how = how;
    event.status = how == BridgeHow::None ? 0 : status;
    std::string incident;
    if (how == BridgeHow::Signal || how == BridgeHow::Exception) {
        incident = mintIncident();
        event.identity.id = incident;
    }
    sink(event);
    return incident;
}

std::uint64_t announcedImage(std::uint32_t cell) noexcept {
    return cell < kMaxInstances ? gImages[cell].load(std::memory_order_relaxed) : 0;
}

void announceState(std::uint32_t cell, InstanceState state) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock);
    if (!sink || cell >= kMaxInstances) return;
    Event event;
    event.event = EventKind::State;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.state = state;
    sink(event);
}

void announceChild(std::uint32_t cell, std::uint32_t childCell, std::uint16_t childGeneration,
                   std::uint64_t childImage) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock);
    if (!sink || cell >= kMaxInstances || childCell >= kMaxInstances) return;
    Event event;
    event.event = EventKind::Child;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.image = childImage;
    event.childCell = childCell;
    event.childGeneration = childGeneration;
    sink(event);
}

void announceSlot(std::uint16_t bridge, std::intptr_t resource) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock);
    if (!sink || bridge == 0 || resource < 0) return;
    Event event;
    event.event = EventKind::Slot;
    event.bridge = bridge;
    event.resource = resource;
    sink(event);
}

std::uint16_t cellGeneration(std::uint32_t cell) noexcept {
    return cell < kMaxInstances ? generationOfCell(cell) : std::uint16_t{0};
}

void announceRemove(std::uint32_t cell) noexcept {
    std::unique_lock<std::mutex> lock;
    const auto sink = enter(lock);
    if (!sink || cell >= kMaxInstances) return;
    Event event;
    event.event = EventKind::Remove;
    event.cell = cell;
    event.generation = generationOfCell(cell);
    event.state = InstanceState::Destroyed;
    sink(event);
}

void announceCommandNames(std::span<const std::string_view> names) noexcept {
    const std::size_t count = std::min(names.size(), kMaxCommandNames);
    for (std::size_t first = 0; first < count; first += kNamesPerChunk) {
        std::string_view chunk[kNamesPerChunk]{};
        bool any = false;
        for (std::size_t i = 0; i < kNamesPerChunk && first + i < count; ++i)
            if (validCommandName(names[first + i])) {
                chunk[i] = names[first + i];
                any = true;
            }
        if (!any) continue;
        Event event;
        event.event = EventKind::Names;
        event.cell = static_cast<std::uint32_t>(first);
        event.identity = {chunk[0], chunk[1], chunk[2], chunk[3]};
        std::unique_lock<std::mutex> lock;
        const auto sink = enter(lock);
        if (!sink) return;
        sink(event);
    }
}

}  // namespace supple::diagctx
