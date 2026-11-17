// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/diagctx/table.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

namespace supple::diagctx {
namespace {
// Zero-initialised and alignas a page: it lands in .bss and is never copied into the
// binary.
alignas(4096) Table gTable;
std::atomic<ThreadHook> gHook{nullptr};

// Releases the thread's slot when the thread ends, so a later thread that reuses the id
// is not labelled with this one's role.
struct Registration {
    int slot = -1;
    ~Registration() {
        if (slot < 0) return;
        auto& entry = gTable.threads[slot];
        entry.id.store(0, std::memory_order_release);
        entry.info.store(0, std::memory_order_relaxed);
    }
};
}  // namespace

const char* threadRoleName(ThreadRole role) noexcept {
    switch (role) {
        case ThreadRole::Model:
            return "model";
        case ThreadRole::AudioRender:
            return "audio_render";
        case ThreadRole::AudioLive:
            return "audio_live";
        case ThreadRole::AudioBackground:
            return "audio_background";
        case ThreadRole::AudioOffline:
            return "audio_offline";
        case ThreadRole::Recorder:
            return "recorder";
        case ThreadRole::MidiOutput:
            return "midi_output";
        case ThreadRole::None:
        case ThreadRole::Count_:
            break;
    }
    return nullptr;
}

Table& table() noexcept { return gTable; }

void initTable() noexcept {
    // Version first, magic last: a reader that sees the magic sees a complete header.
    gTable.header.version.store(kTableVersion, std::memory_order_relaxed);
    gTable.header.magic.store(kTableMagic, std::memory_order_release);
}

bool imageValid(const TableImage& image) noexcept {
    return image.header.magic == kTableMagic && image.header.version == kTableVersion;
}

SlotInfo slotInfo(std::uint64_t info) noexcept {
    SlotInfo result;
    const auto role = static_cast<std::uint8_t>(info & 0xff);
    if (role == 0 || role >= kThreadRoleCount) return result;
    result.role = static_cast<ThreadRole>(role);
    result.ordinal = static_cast<std::uint16_t>((info >> 8) & 0xffff);
    return result;
}

SlotInfo findThread(const TableImage& image, std::uint64_t id) noexcept {
    if (id == 0 || id == kClaiming) return {};
    for (const auto& slot : image.threads)
        if (slot.id == id) return slotInfo(slot.info);
    return {};
}

SlotInfo findThread(std::uint64_t id) noexcept {
    if (id == 0 || id == kClaiming) return {};
    for (const auto& slot : gTable.threads)
        if (slot.id.load(std::memory_order_acquire) == id)
            return slotInfo(slot.info.load(std::memory_order_relaxed));
    return {};
}

std::uint64_t currentThreadId() noexcept {
#if defined(_WIN32)
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(__APPLE__)
    // The name task_threads lists this thread under. pthread_mach_thread_np adds no
    // reference to the port, so nothing is leaked and nothing has to be released.
    return static_cast<std::uint64_t>(::pthread_mach_thread_np(::pthread_self()));
#else
    return 0;
#endif
}

bool registerThread(ThreadRole role, std::uint16_t ordinal) noexcept {
    if (role == ThreadRole::None || role >= ThreadRole::Count_) return false;
    thread_local Registration mine;
    if (mine.slot >= 0) return true;
    initTable();
    const std::uint64_t id = currentThreadId();
    if (id == 0) return false;
    const std::uint64_t info = static_cast<std::uint64_t>(role) | std::uint64_t{ordinal} << 8;
    for (std::size_t i = 0; i < kMaxThreadSlots; ++i) {
        auto& slot = gTable.threads[i];
        // Claim a free slot (id 0). The role goes in before the id is published, so a
        // reader never pairs a new id with an old role.
        std::uint64_t expected = 0;
        if (slot.id.load(std::memory_order_relaxed) != 0) continue;
        // Reserve with a sentinel nobody uses as an id, then publish.
        if (!slot.id.compare_exchange_strong(expected, kClaiming, std::memory_order_acquire,
                                             std::memory_order_relaxed))
            continue;
        slot.info.store(info, std::memory_order_relaxed);
        slot.id.store(id, std::memory_order_release);
        mine.slot = static_cast<int>(i);
        if (const ThreadHook hook = gHook.load(std::memory_order_acquire)) hook();
        return true;
    }
    gTable.header.threadsDropped.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void setThreadHook(ThreadHook hook) noexcept { gHook.store(hook, std::memory_order_release); }

}  // namespace supple::diagctx
